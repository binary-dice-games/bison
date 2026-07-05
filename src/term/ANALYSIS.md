# ESPECIFICACIÓN TÉCNICA: COMUNICACIÓN BIDIRECCIONAL ASÍNCRONA VÍA LIBUV EN WINDOWS MEDIANTE SECUENCIAS DE ESCAPE

## 1. Introducción y Arquitectura del Sistema

Este documento define la arquitectura y los detalles de implementación para establecer un canal de comunicación bidireccional, asíncrono e invisible entre un proceso anfitrión (padre) y un proceso subordinado (hijo) a través de streams estándar (`stdin`/`stdout`).

A diferencia de un pipeline tradicional que rompe la interactividad del shell, esta arquitectura implementa un **enfoque basado en Proxy intermedio**. El proceso padre utiliza `libuv` para monitorizar asíncronamente las entradas del usuario y las salidas del hijo, interceptando exclusivamente metadatos específicos codificados mediante secuencias de escape OSC (Operating System Command). El resto del tráfico fluye de manera transparente, manteniendo la consola completamente interactiva.

```
       +-----------------------------------------------------------+
       |                  CONSOLA NATIVA (CONPTY)                  |
       +-----------------------------------------------------------+
                        │ (Teclas)               ▲ (Texto / ANSI)
                        ▼                        │
       +-----------------------------------------------------------+
       |               PROCESO ANFITRIÓN (LIBUV)                   |
       |                                                           |
       |    uv_pipe_t (stdin)               uv_pipe_t (stdout)     |
       |    Manejador Entrada               Parser OSC 99 / BEL    |
       +-----------------------------------------------------------+
                        │                        ▲
                        │ ttywrite()             │ std::cout
                        ▼                        │
       +-----------------------------------------------------------+
       |                PROCESO SUBORDINADO (HIJO)                 |
       |                  (Shell o App de Consola)                 |
       +-----------------------------------------------------------+

```

---

## 2. Definición del Protocolo de Datos

Para evitar colisiones visuales con códigos ANSI estándar (colores, cursor) y prevenir que los metadatos ensucien la interfaz visual, se implementa una transmisión simétrica:

### A. Dirección Hijo $\rightarrow$ Padre (Telemetría/Metadatos)

Se encapsula en una secuencia personalizada OSC 99. Estas secuencias son idóneas porque los emuladores de terminal las descartan del renderizado gráfico si no las reconocen.

* **Formato:** `ESC ] 99 ; <datos_payload> BEL`
* **Representación en bytes:** `\033]99;<datos>\a` (ó `\x1b]99;<datos>\x07`)

### B. Dirección Padre $\rightarrow$ Hijo (Comandos Programáticos)

Inyección de texto plano simulando la entrada física del teclado en el canal de entrada del proceso subordinado, utilizando un prefijo único discriminador.

* **Formato:** `__IMGUI_DATA__:<comando>\n`

---

## 3. Implementación del Proceso Anfitrión (Padre) con `libuv`

El proceso padre debe inicializar el bucle de eventos de `libuv` (`uv_loop_t`), configurar los pipes asíncronos y lanzar el proceso hijo configurando sus descriptores de archivo de forma interactiva.

### 3.1. Configuración de Pipes y Estructura `uv_process_options_t`

Para mantener la terminal interactiva, `stdio[2]` (stderr) se hereda directamente o se expone, mientras que `stdin` y `stdout` se gestionan mediante pipes asíncronos de `libuv`.

```cpp
// Inicialización de estructuras
uv_loop_t* loop = uv_default_loop();
uv_pipe_t child_stdin;
uv_pipe_t child_stdout;

uv_pipe_init(loop, &child_stdin, 0);
uv_pipe_init(loop, &child_stdout, 0);

// Opciones del proceso hijo
uv_process_options_t options = {0};
options.exit_cb = on_child_exit;
#ifdef _WIN32
options.file = "cmd.exe"; // O PowerShell.exe según el entorno
#else
options.file = "/bin/sh";
#endif

// Configuración de contenedores STDIO
uv_stdio_container_t stdio[3];

// STDIN del hijo: El padre escribe de forma asíncrona
stdio[0].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_READABLE_PIPE);
stdio[0].data.stream = (uv_stream_t*)&child_stdin;

// STDOUT del hijo: El padre lee e intercepta la telemetría
stdio[1].flags = (uv_stdio_flags)(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
stdio[1].data.stream = (uv_stream_t*)&child_stdout;

// STDERR del hijo: Hereda el flujo actual de la consola para diagnósticos directos
stdio[2].flags = UV_INHERIT_FD;
stdio[2].data.fd = 2; 

options.stdio_count = 3;
options.stdio = stdio;

uv_process_t child_req;
int r = uv_spawn(loop, &child_req, &options);
if (r < 0) {
    fprintf(stderr, "Error al lanzar proceso: %s\n", uv_strerror(r));
    return 1;
}

```

### 3.2. Máquina de Estados del Parser en el Bucle de Lectura

Cuando el hijo produce salida, `libuv` invoca el callback asociado a `uv_read_start` en `child_stdout`. El agente debe implementar una máquina de estados para discriminar el texto plano de la secuencia de escape OSC 99.

```cpp
enum ParserState {
    STATE_NORMAL,
    STATE_ESC,
    STATE_OSC,
    STATE_CAPTURING_METADATA
};

ParserState current_state = STATE_NORMAL;
std::string metadata_buffer = "";

void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

void on_child_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        std::string normal_output = "";
        
        for (ssize_t i = 0; i < nread; i++) {
            char c = buf->base[i];
            
            switch (current_state) {
                case STATE_NORMAL:
                    if (c == '\033') { // ESC
                        current_state = STATE_ESC;
                    } else {
                        normal_output += c;
                    }
                    break;
                    
                case STATE_ESC:
                    if (c == ']') { // Inicio de OSC
                        current_state = STATE_OSC;
                    } else {
                        // No era un comando OSC, restaurar y tratar como texto normal
                        normal_output += '\033';
                        normal_output += c;
                        current_state = STATE_NORMAL;
                    }
                    break;
                    
                case STATE_OSC:
                    // Verificamos si comienza con nuestro identificador '99;'
                    metadata_buffer += c;
                    if (metadata_buffer == "99;") {
                        metadata_buffer.clear();
                        current_state = STATE_CAPTURING_METADATA;
                    } else if (metadata_buffer.length() >= 3) {
                        // No es nuestra secuencia OSC 99, devolver los bytes al flujo normal
                        normal_output += "\033]";
                        normal_output += metadata_buffer;
                        metadata_buffer.clear();
                        current_state = STATE_NORMAL;
                    }
                    break;
                    
                case STATE_CAPTURING_METADATA:
                    if (c == '\a') { // BEL (Fin de secuencia)
                        // DISPARAR EVENTO INTERNO CON EL METADATO PROCESADO
                        ProcesarMetadatoInterno(metadata_buffer);
                        metadata_buffer.clear();
                        current_state = STATE_NORMAL;
                    } else {
                        metadata_buffer += c;
                    }
                    break;
            }
        }
        
        // Escribir de vuelta a la terminal del usuario el texto limpio
        if (!normal_output.empty()) {
            // Nota: Aquí se utiliza la escritura asíncrona de libuv hacia el stdout real (fd 1)
            ImprimirEnPantallaUsuario(normal_output);
        }
    }
    
    if (buf->base) free(buf->base);
}

```

### 3.3. Entrada de Usuario Interactiva e Inyección de Comandos

Para que el usuario interactúe con el shell de forma nativa, se debe abrir un flujo `uv_pipe_t` ligado al descriptor de entrada estándar del sistema (`fd 0`). Cualquier entrada capturada se reenvía íntegramente a `child_stdin`.

Cuando la lógica de negocio del padre requiera forzar un comando, se utiliza la función de escritura inyectando el prefijo de control:

```cpp
void EnviarComandoAlHijo(uv_stream_t* child_stdin_stream, const std::string& comando) {
    std::string payload = "__IMGUI_DATA__:" + comando + "\n"; //
    
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    uv_buf_t buf = uv_buf_init((char*)payload.c_str(), payload.length());
    
    // Escritura asíncrona no bloqueante
    uv_write(write_req, child_stdin_stream, &buf, 1, [](uv_write_t* req, int status) {
        free(req);
    });
}

```

---

## 4. Implementación del Proceso Subordinado (Hijo)

El proceso hijo expone una lógica simétrica: mantiene un hilo secundario de escucha en `stdin` sin bloquear las tareas principales de cómputo del hilo primario.

### Flujo de Trabajo en C++ (Estructura de Referencia para el Agente)

```cpp
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

void HiloEscuchaInyeccionPadre() {
    std::string linea;
    // Lectura bloqueante delegada en un hilo secundario dedicado
    while (std::getline(std::cin, linea)) {
        // Validar el prefijo del protocolo
        if (linea.rfind("__IMGUI_DATA__:", 0) == 0) { //
            std::string comando = linea.substr(15);   // Extraer carga útil
            
            // Lógica de despacho de comandos
            if (comando == "SET_PERFORMANCE_HIGH") { //
                // Modificar estado interno de la app del hijo
            }
        }
    }
}

int main() {
    // Inicializar el canal de entrada asíncrono
    std::thread hilo_escucha(HiloEscuchaInyeccionPadre);
    hilo_escucha.detach(); // Separar para ejecución independiente

    // Bucle principal de ejecución del proceso hijo
    while (true) {
        // ... Ejecución de procesos de negocio ...
        
        // Emisión asíncrona de telemetría invisible al usuario
        int progreso_actual = 82;
        std::cout << "\033]99;PROGRESO:" << progreso_actual << "\a" << std::flush; //
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); //
    }
    return 0;
}

```

---

## 5. Instrucciones Críticas para el Agente de Programación

Al implementar esta especificación, el agente debe asegurar el cumplimiento estricto de los siguientes puntos:

1. **Gestión de Memoria en Libuv:** Asegurar que los búferes asignados en el callback `alloc_buffer` sean liberados inequívocamente en `on_child_read` mediante `free(buf->base)` para evitar fugas de memoria críticas en transmisiones de alta frecuencia.
2. **Configuración del Entorno de Consola:** En entornos de Windows, inyectar explícitamente la variable de entorno `SetEnvironmentVariableA("MSYS", "enable_pcon");` antes de invocar `uv_spawn` para forzar la correcta cooperación de flujos VT virtuales puros a través de ConPTY.
3. **Preservación del Estado Parcial:** El buffer del parser (`metadata_buffer`) y el estado actual de la máquina (`current_state`) deben ser persistentes entre llamadas sucesivas de `on_child_read`, dado que un paquete de escape puede llegar fragmentado a través de diferentes eventos de lectura de red/pipe de `libuv`.
4. **Desactivación de Modos de Consola de Windows:** Configurar de manera explícita el modo de la terminal de entrada mediante las APIs nativas de Windows (`SetConsoleMode`) desactivando `ENABLE_PROCESSED_INPUT` para evitar que secuencias de teclas de control del usuario rompan prematuramente la ejecución del bucle de eventos.
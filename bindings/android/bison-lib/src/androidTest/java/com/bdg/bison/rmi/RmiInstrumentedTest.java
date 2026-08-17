// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.fail;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import com.bdg.bison.Dynamic;
import org.junit.Test;
import org.junit.runner.RunWith;

/**
 * RMI-specific coverage of the async ({@code rmi_future_handle}), proxy
 * event, and server auth-handler surface -- the counterpart to {@code
 * DynamicInstrumentedTest}'s {@code rmiStandaloneRoundTrip} for the parts
 * of {@code rmi_c.h} that need more than a single in-process round trip to
 * exercise. Runs on-device the same way (see docs/examples.md).
 */
@RunWith(AndroidJUnit4.class)
public class RmiInstrumentedTest {

  @Test
  public void asyncInstantiateAndCallRoundTrip() {
    try (Dynamic proto = new Dynamic("InstrumentedAsyncCalculator")) {
      proto.addMethod("add", (self, params, result) -> result.setInt("value", params.getInt("a") + params.getInt("b")));
      Dynamic.registerClass(proto, null, null, null);
    }

    try (Client client = Client.standalone()) {
      client.connect();

      Proxy proxy;
      try (Future instantiateFuture = client.instantiateAsync("InstrumentedAsyncCalculator", null)) {
        instantiateFuture.waitFor();
        proxy = instantiateFuture.getProxy();
      }
      assertNotNull(proxy);

      try (proxy) {
        try (Dynamic args = new Dynamic()) {
          args.setInt("a", 5);
          args.setInt("b", 6);
          try (Future callFuture = proxy.callAsync("add", args)) {
            callFuture.waitFor();
            try (Dynamic result = callFuture.getDynamic()) {
              assertEquals(11, result.getInt("value"));
            }
          }
        }
      }
      client.disconnect();
    }
  }

  @Test
  public void proxyOnEventRegistrationSucceeds() {
    try (Dynamic proto = new Dynamic("InstrumentedEventCalculator")) {
      Dynamic.registerClass(proto, null, null, null);
    }

    try (Client client = Client.standalone()) {
      client.connect();
      try (Proxy proxy = client.instantiate("InstrumentedEventCalculator", null)) {
        // No server-side event source is reachable from this pure-C-ABI
        // binding (rmi_c.h exposes no "emit event" call), so this is a
        // smoke test: registration itself must not throw.
        proxy.onEvent("changed", params -> {});
      }
      client.disconnect();
    }
  }

  @Test
  public void tcpServerAuthHandlerAcceptsAndRejects() {
    int port = 19343;
    try (Dynamic proto = new Dynamic("InstrumentedTcpEcho")) {
      proto.addFieldInt("value", 0, null);
      Dynamic.registerClass(proto, null, null, null);
    }

    try (Server server = Server.tcp("127.0.0.1", port)) {
      server.listen(null, payload -> "accept".equals(payload.getString("mode")) ? "trusted-client" : null);

      try (Client accepted = Client.tcp("127.0.0.1", port)) {
        try (Dynamic connectParams = new Dynamic()) {
          connectParams.setString("mode", "accept");
          accepted.connect(connectParams);
          try (Proxy proxy = accepted.instantiate("InstrumentedTcpEcho", null)) {
            try (Dynamic snapshot = proxy.get()) {
              assertEquals(0, snapshot.getInt("value"));
            }
          }
        }
        accepted.disconnect();
      }

      try (Client rejected = Client.tcp("127.0.0.1", port)) {
        try (Dynamic connectParams = new Dynamic()) {
          connectParams.setString("mode", "reject");
          try {
            rejected.connect(connectParams);
            rejected.instantiate("InstrumentedTcpEcho", null).close();
            fail("expected the auth handler's rejection to surface as an RmiException");
          } catch (RmiException expected) {
            // The auth handler returned null for this payload -- rejected.
          }
        }
      }

      server.stop();
    }
  }
}

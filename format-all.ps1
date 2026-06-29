$ClangPath = "C:\Program Files\LLVM\bin\clang-format.exe"
# Add any folder names you want to exclude here
$ExcludedFolders = @("extern", "build")

# Converts the list into a regex pattern (e.g., \\(extern|build)\\)
$ExcludePattern = "\\(" + ($ExcludedFolders -join "|") + ")\\"

Get-ChildItem -Recurse -Include *.cpp, *.h, *.hpp, *.c, *.cc | ForEach-Object {
    # Skip the file if its path matches any excluded folder
    if ($_.FullName -match $ExcludePattern) {
        return
    }
    
    & $ClangPath -i $_.FullName
    Write-Host "Formatted: $($_.Name)" -ForegroundColor Green
}

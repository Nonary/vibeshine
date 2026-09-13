# Sunshine Playnite Connector

This is the compiled Playnite 11 replacement for the retired PowerShell script extension.

## Build

Build `SunshinePlaynite.csproj` in Release mode, then package the contents of
`bin/Release/net462` as a Playnite extension with Toolbox:

```powershell
dotnet build SunshinePlaynite.csproj -c Release
Toolbox.exe pack .\bin\Release\net462 .
```

For local testing, add `bin/Release/net462` as a developer plugin in Playnite's
`Settings -> For developers` page. Remove the old PowerShell extension before
testing or installing this compiled replacement.

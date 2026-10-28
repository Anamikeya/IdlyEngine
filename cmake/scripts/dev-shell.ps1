<#
.SYNOPSIS
  Runs a command inside the x64 MSVC developer environment so Ninja can find cl.exe.

.EXAMPLE
  ./cmake/scripts/dev-shell.ps1 cmake --preset windows-msvc-debug
  ./cmake/scripts/dev-shell.ps1 cmake --build --preset windows-msvc-debug
  ./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug
#>
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Command)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio 2022 with the C++ workload.' }
	$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if (-not $vs) { throw 'No Visual Studio installation with the MSVC x64 toolset was found.' }
	$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
	# vcvars64.bat itself looks vswhere.exe up on PATH.
	$env:PATH = "$(Split-Path $vswhere);$env:PATH"
	# Import the environment produced by vcvars64.bat into this PowerShell process.
	cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
		if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) }
	}
}

if ($Command.Count -gt 0) {
	# 1..0 counts down in PowerShell, so a single-word command needs an explicit empty argument list.
	$arguments = if ($Command.Count -gt 1) { $Command[1..($Command.Count - 1)] } else { @() }
	& $Command[0] @arguments
	exit $LASTEXITCODE
}

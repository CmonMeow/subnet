param([switch]$PublishOnly)
$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
if(!$vs){throw 'Visual Studio not found.'}
if(!$PublishOnly){
    $header=Get-Content UpdateRelease.h -Raw
    $version=[uint64]([regex]::Match($header,'UPDATE_RELEASE (\d+)').Groups[1].Value)+1
    Set-Content UpdateRelease.h "#pragma once`n#define UPDATE_RELEASE ${version}L`n#ifndef RC_INVOKED`ninline constexpr unsigned long long UpdateRelease=UPDATE_RELEASE;`n#endif" -Encoding ascii
    & "$vs\MSBuild\Current\Bin\MSBuild.exe" oi.vcxproj /m /p:Configuration=Release /p:Platform=x64 /v:minimal
    if($LASTEXITCODE){throw 'Client build failed; no update published.'}
}
$version=[uint64]([regex]::Match((Get-Content UpdateRelease.h -Raw),'UPDATE_RELEASE (\d+)').Groups[1].Value)
$command='@call "'+$vs+'\VC\Auxiliary\Build\vcvars64.bat" >nul'+"`r`n"+'@cl /nologo /O2 /EHsc /std:c++17 /MT /DSODIUM_STATIC /I. /Ithird_party/libsodium/include PublishUpdate.cpp /Fobuild/PublishUpdate.obj /Febuild/PublishUpdate.exe /link third_party/libsodium/lib/x64/Release/libsodium.lib advapi32.lib crypt32.lib ws2_32.lib'
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'build\publisher.cmd'),$command)
cmd /c build\publisher.cmd
if($LASTEXITCODE){throw 'Publisher build failed.'}
& .\build\PublishUpdate.exe $version
if($LASTEXITCODE){throw 'Manifest signing failed.'}

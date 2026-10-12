param([string]$Path=(Join-Path $PSScriptRoot 'private\update-signing.backup'),[switch]$Restore)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
$certificate=& (Join-Path $PSScriptRoot 'GetSigningCertificate.ps1')
$keyPath=Join-Path $PSScriptRoot 'private\update.key'
$secret=$null
try {
    if($Restore){
        if(Test-Path -LiteralPath $keyPath){throw 'An update key already exists; move it aside before restoring.'}
        $backup=Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        if($backup.thumbprint -ne $certificate.Thumbprint){throw 'Import the original signing certificate/PFX before restoring this backup.'}
        $rsa=[System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
        try {$secret=$rsa.Decrypt([Convert]::FromBase64String($backup.ciphertext),[System.Security.Cryptography.RSAEncryptionPadding]::OaepSHA256)} finally {$rsa.Dispose()}
        if($secret.Length -ne 64){throw 'Invalid update signing key.'}
        $protected=[System.Security.Cryptography.ProtectedData]::Protect($secret,$null,[System.Security.Cryptography.DataProtectionScope]::CurrentUser)
        [IO.File]::WriteAllBytes($keyPath,$protected)
        Write-Output 'Restored update key for this Windows account.'
    } else {
        if(Test-Path -LiteralPath $Path){throw 'Backup already exists; choose another path.'}
        $secret=[System.Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($keyPath),$null,[System.Security.Cryptography.DataProtectionScope]::CurrentUser)
        $rsa=[System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($certificate)
        try {$ciphertext=$rsa.Encrypt($secret,[System.Security.Cryptography.RSAEncryptionPadding]::OaepSHA256)} finally {$rsa.Dispose()}
        @{thumbprint=$certificate.Thumbprint;ciphertext=[Convert]::ToBase64String($ciphertext)} | ConvertTo-Json | Set-Content -LiteralPath $Path -Encoding ascii
        Write-Output "Encrypted backup written to $Path. Keep it with your password-protected signing-certificate PFX backup."
    }
} finally {if($secret){[Array]::Clear($secret,0,$secret.Length)}}

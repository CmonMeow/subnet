param([string]$Path=(Join-Path $PSScriptRoot 'private\oi-signing-backup.pfx'))
$ErrorActionPreference='Stop'
$expected=(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'SigningThumbprint.txt') -Raw).Trim()
if($expected -notmatch '^[0-9A-Fa-f]{40}$'){throw 'Invalid signing thumbprint.'}
$password=Read-Host 'Enter the signing-backup PFX password' -AsSecureString
$certificate=$null
$store=$null
try {
    $bytes=[IO.File]::ReadAllBytes($Path)
    $certificate=[System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $bytes,$password,[System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    if($certificate.Thumbprint -ne $expected -or !$certificate.HasPrivateKey){
        throw 'This backup does not contain the expected signing private key.'
    }
    $certificate.Dispose()
    $flags=[System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::UserKeySet -bor
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::PersistKeySet -bor
        [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable
    $certificate=[System.Security.Cryptography.X509Certificates.X509Certificate2]::new($bytes,$password,$flags)
    $store=[System.Security.Cryptography.X509Certificates.X509Store]::new('My','CurrentUser')
    $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    $store.Add($certificate)
    $rsa=[System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($certificate)
    if(!$rsa){throw 'The private key was not restored.'}
    try {
        $null=$rsa.SignData([byte[]](1,2,3),[System.Security.Cryptography.HashAlgorithmName]::SHA256,[System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    } finally {$rsa.Dispose()}
    Write-Output 'Signing key restored.'
} finally {
    if($store){$store.Dispose()}
    if($certificate){$certificate.Dispose()}
    $password.Dispose()
}
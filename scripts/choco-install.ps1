# Installs a Chocolatey package, retrying while the community feed is unavailable.
# choco can exit 0 after "Unable to find package", so success also requires the
# package to be listed as installed.
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Version,
    [string]$PackageParameters
)

$chocoArgs = @('install', $Package, '-y')
if ($Version) { $chocoArgs += "--version=$Version" }
if ($PackageParameters) { $chocoArgs += "--params=$PackageParameters" }

$attempts = 3
for ($attempt = 1; $attempt -le $attempts; $attempt++) {
    & choco @chocoArgs
    if ($LASTEXITCODE -eq 0 -and (choco list --exact $Package --limit-output)) {
        exit 0
    }
    if ($attempt -lt $attempts) {
        Write-Host "choco install $Package failed (attempt $attempt of $attempts), retrying in 60s"
        Start-Sleep -Seconds 60
    }
}
Write-Host "choco install $Package failed after $attempts attempts"
exit 1

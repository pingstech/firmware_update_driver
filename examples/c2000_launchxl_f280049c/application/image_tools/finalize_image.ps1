# Called by make_image.bat. Takes the WIDE image produced by hex2000 (app_bin.hexcfg: 0x20000 bytes, larger than the capacity), trims the trailing 0xFF padding
# and rounds the size up to a multiple of the write granule (16 bytes): the image SIZE IS ALWAYS the real size of the application.
#
# WHY THIS METHOD: hex2000 SILENTLY truncates data that exceeds the ROMS length. A fixed / hand-written length truncated the image when the application grew
# and the device did not start after the update. Here the length is not written by hand anywhere; because ROMS is LARGER than the capacity, an application
# that exceeds the capacity shows up as real data and the check below STOPS the build.
param([string]$Name = "app_test.bin")

$capacity = 0x12000        # staging image capacity: 9 sectors x 8192 = 73728 bytes (layout_f280049c.h: staging - 1 manifest page)
$granule  = 16             # flash write granule (bytes)
$path     = Join-Path $PSScriptRoot $Name

if(-not (Test-Path $path)) { Write-Host "ERROR: $Name was not produced (hex2000 failed)."; exit 1 }
$bytes = [IO.File]::ReadAllBytes($path)
$last  = $bytes.Length - 1
while(($last -ge 0) -and ($bytes[$last] -eq 0xFF)) { $last-- }
if($last -lt 0) { [IO.File]::Delete($path); Write-Host "ERROR: image is empty (no flash data in the .out)."; exit 1 }

$len = [int]([math]::Ceiling(($last + 1) / [double]$granule) * $granule)
if($len -gt $capacity) {
    [IO.File]::Delete($path)
    Write-Host ("ERROR: application is {0} bytes, image capacity is {1} bytes. IMAGE NOT PRODUCED (a truncated image would leave the device unable to start)." -f $len, $capacity)
    Write-Host "       Fix: enable optimization (-O2), remove unneeded code, or increase the number of staging/application sectors."
    exit 1
}
$out = New-Object byte[] $len
[Array]::Copy($bytes, $out, $len)
[IO.File]::WriteAllBytes($path, $out)
Write-Host ("Image: {0} bytes (capacity {1}, {2:N1}%), remaining margin {3} bytes" -f $len, $capacity, (100.0 * $len / $capacity), ($capacity - $len))
exit 0

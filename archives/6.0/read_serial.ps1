$port = new-Object System.IO.Ports.SerialPort('COM4',115200,'None','8','one')
$port.Open()
Start-Sleep 3
$data = $port.ReadExisting()
Write-Output "=== SERIAL OUTPUT ==="
Write-Output $data
$port.Close()

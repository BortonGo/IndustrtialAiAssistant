# Local Windows OCR; stdout is one UTF-8 JSON object. No downloads or settings changes.
param([Parameter(Mandatory=$true)][string]$ImagePath)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType=WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType=WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Foundation, ContentType=WindowsRuntime]
$null = [Windows.Globalization.Language, Windows.Globalization, ContentType=WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
function Await($operation, [Type]$resultType) {
    $task = $asTask.MakeGenericMethod($resultType).Invoke($null, @($operation))
    if (-not $task.Wait(15000)) { throw 'Windows OCR timed out' }
    return $task.Result
}
$stream = $null
$bitmap = $null
try {
    # Russian recognizes Latin too. Without it, do not silently misread Cyrillic as Latin.
    $language = [Windows.Globalization.Language]::new('ru')
    $engine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromLanguage($language)
    if ($null -eq $engine) { throw 'Russian Windows OCR language pack is unavailable' }
    $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($ImagePath)) ([Windows.Storage.StorageFile])
    $stream = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
    $decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
    if ([Math]::Max($decoder.PixelWidth, $decoder.PixelHeight) -gt [Windows.Media.Ocr.OcrEngine]::MaxImageDimension) {
        throw 'Image exceeds Windows OCR dimension limit'
    }
    $bitmap = Await ($decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
    $result = Await ($engine.RecognizeAsync($bitmap)) ([Windows.Media.Ocr.OcrResult])
    $lines = @($result.Lines | ForEach-Object {
        @{text=$_.Text; words=@($_.Words | ForEach-Object {
            $r = $_.BoundingRect
            @{text=$_.Text; x=$r.X; y=$r.Y; width=$r.Width; height=$r.Height}
        })}
    })
    @{engine='windows-ocr-ru-v1'; lines=$lines; angle=$result.TextAngle} | ConvertTo-Json -Depth 8 -Compress
} catch {
    @{error=$_.Exception.Message} | ConvertTo-Json -Compress
    exit 1
} finally {
    if ($null -ne $bitmap) { $bitmap.Dispose() }
    if ($null -ne $stream) { $stream.Dispose() }
}

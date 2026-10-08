param([Parameter(Mandatory=$true)][string]$InputPath,
      [Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

# A private Word instance is guarded by a Job Object even if our parent is stopped.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WordProcessGuard {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateJobObject(IntPtr a, string n);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr j, int c, IntPtr i, uint s);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr j, IntPtr p);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool i, uint p);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    public static IntPtr Guard(uint pid) {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        // JOBOBJECT_EXTENDED_LIMIT_INFORMATION (Windows ABI).
        int size = IntPtr.Size == 8 ? 144 : 112;
        IntPtr info = Marshal.AllocHGlobal(size);
        for (int n=0; n<size; n++) Marshal.WriteByte(info,n,0);
        Marshal.WriteInt32(info,16,0x2000); // KILL_ON_JOB_CLOSE
        bool ok = SetInformationJobObject(job,9,info,(uint)size);
        Marshal.FreeHGlobal(info);
        IntPtr process = OpenProcess(0x0101,false,pid);
        ok = ok && process != IntPtr.Zero && AssignProcessToJobObject(job,process);
        if (process != IntPtr.Zero) CloseHandle(process);
        if (!ok) { CloseHandle(job); throw new Exception("Cannot isolate Word conversion process"); }
        return job;
    }
}
'@
$existingWord = @(Get-Process WINWORD -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$word = $null
$document = $null
$guard = [IntPtr]::Zero
$owned = $false
$previousUpdateLinks = $null
try {
    $word = New-Object -ComObject Word.Application
    $newWord = @(Get-Process WINWORD -ErrorAction SilentlyContinue | Where-Object { $existingWord -notcontains $_.Id })
    if ($newWord.Count -ne 1) {
        throw 'Word did not create a separate instance. Close Word and retry DOC import.'
    }
    $owned = $true
    $guard = [WordProcessGuard]::Guard([uint32]$newWord[0].Id)
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $word.AutomationSecurity = 3 # Disable macros before opening the input.
    $previousUpdateLinks = $word.Options.UpdateLinksAtOpen
    $word.Options.UpdateLinksAtOpen = $false
    $document = $word.Documents.Open($InputPath, $false, $true, $false)
    $document.SaveAs2($OutputPath, 16, $false, '', $false) # DOCX; do not add to recent files.
} finally {
    try {
        $doNotSave = 0
        if ($null -ne $document) {
            $document.Close([ref]$doNotSave)
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($document)
        }
        if ($null -ne $word) {
            if ($owned) {
                if ($null -ne $previousUpdateLinks) { $word.Options.UpdateLinksAtOpen = $previousUpdateLinks }
                $word.Quit([ref]$doNotSave)
            }
            [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($word)
        }
    } finally {
        if ($guard -ne [IntPtr]::Zero) { [void][WordProcessGuard]::CloseHandle($guard) }
    }
}

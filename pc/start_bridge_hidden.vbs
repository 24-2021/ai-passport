Option Explicit

Dim fso, sh, here, repo, logDir, logFile, q, cmd
Dim attempt, delaySeconds, startedAt, elapsed, rc, i

Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")

here = fso.GetParentFolderName(WScript.ScriptFullName)
repo = fso.GetParentFolderName(here)
logDir = sh.ExpandEnvironmentStrings("%LOCALAPPDATA%") & "\kfb24"
If Not fso.FolderExists(logDir) Then fso.CreateFolder logDir
logFile = logDir & "\bridge.log"
On Error Resume Next
If fso.FileExists(logFile) Then
    If fso.GetFile(logFile).Size > 5 * 1024 * 1024 Then fso.DeleteFile logFile
End If
Err.Clear
On Error GoTo 0

If BridgeRunning() Then
    AppendLog "kfb24 supervisor: bridge already running, exiting"
    WScript.Quit 0
End If

sh.CurrentDirectory = repo
q = Chr(34)
cmd = "cmd.exe /c " & q & q & here & "\start_bridge.cmd" & q & _
      " --autostart >> " & q & logFile & q & " 2>&1" & q

attempt = 0
Do
    startedAt = Timer
    rc = sh.Run(cmd, 0, True)
    If rc = 0 Then Exit Do
    elapsed = Timer - startedAt
    If elapsed < 0 Then elapsed = elapsed + 86400
    If elapsed >= 60 Then attempt = 0
    attempt = attempt + 1
    delaySeconds = 5
    For i = 2 To attempt
        If delaySeconds >= 60 Then Exit For
        delaySeconds = delaySeconds * 2
    Next
    AppendLog "kfb24 supervisor: bridge exited with code " & rc & _
              "; restarting in " & delaySeconds & "s"
    WScript.Sleep delaySeconds * 1000
Loop

Sub AppendLog(message)
    On Error Resume Next
    Dim f
    Set f = fso.OpenTextFile(logFile, 8, True)
    f.WriteLine message
    f.Close
    Err.Clear
End Sub

Function BridgeRunning()
    Dim svc, procs, p
    BridgeRunning = False
    On Error Resume Next
    Set svc = GetObject("winmgmts:\\.\root\cimv2")
    Set procs = svc.ExecQuery("SELECT CommandLine FROM Win32_Process WHERE Name='python.exe'")
    For Each p In procs
        If Not IsNull(p.CommandLine) Then
            If InStr(1, p.CommandLine, "kfb24_bridge.py", vbTextCompare) > 0 Then
                BridgeRunning = True
                Exit For
            End If
        End If
    Next
    On Error GoTo 0
End Function

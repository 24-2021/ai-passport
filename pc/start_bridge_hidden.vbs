Option Explicit
Dim fso, sh, here, repo, logDir, logFile, cmd, q
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

sh.CurrentDirectory = repo
q = Chr(34)
cmd = "cmd.exe /c " & q & q & here & "\start_bridge.cmd" & q & _
      " --autostart >> " & q & logFile & q & " 2>&1" & q
sh.Run cmd, 0, False

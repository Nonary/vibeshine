' Displays a simple message after a successful Repair. The optional
' terminal-isolation property can require a Windows restart.

Function ShowRepairComplete()
    Dim msg
    If Session.Property("INSTALL_TERMINAL_ISOLATION") = "1" Then
        msg = "Vibeshine repair completed successfully. A Windows restart is required before terminal isolation can become active."
    Else
        msg = "Vibeshine repair completed successfully. No reboot is required for the selected components."
    End If
    MsgBox msg, vbInformation + vbOKOnly, "Vibeshine Repair"
    ShowRepairComplete = 0
End Function

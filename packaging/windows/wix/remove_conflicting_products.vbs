Const HKEY_CURRENT_USER = &H80000001
Const HKEY_LOCAL_MACHINE = &H80000002

Function RemoveConflictingProducts()
    On Error Resume Next

    Dim reg
    Set reg = GetObject("winmgmts:\\.\root\default:StdRegProv")

    If Err.Number <> 0 Then
        LogMessage "RemoveConflictingProducts: failed to initialize registry provider: " & Err.Description
        RemoveConflictingProducts = 3
        Exit Function
    End If

    Dim hives(1), roots(1)
    hives(0) = HKEY_LOCAL_MACHINE
    hives(1) = HKEY_CURRENT_USER
    roots(0) = "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"
    roots(1) = "SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"

    Dim hive, root, enumResult, subKeys, i
    Dim foundAny
    foundAny = False

    For Each hive In hives
        For Each root In roots
            enumResult = reg.EnumKey(hive, root, subKeys)
            If enumResult = 0 And IsArray(subKeys) Then
                For i = 0 To UBound(subKeys)
                    Dim subKeyName, fullPath, displayName
                    subKeyName = CStr(subKeys(i))
                    fullPath = root & "\" & subKeyName
                    displayName = ReadStringValue(reg, hive, fullPath, "DisplayName")

                    If IsTargetProduct(displayName) Then
                        foundAny = True
                        LogMessage "RemoveConflictingProducts: blocking install because conflicting product remains installed: " & displayName & " (" & fullPath & ")"
                    End If
                Next
            End If
        Next
    Next

    If foundAny Then
        LogMessage "RemoveConflictingProducts: conflicting products must be removed by the bootstrapper or by the user before running the MSI."
        RemoveConflictingProducts = 3
    Else
        LogMessage "RemoveConflictingProducts: no conflicting products detected."
        RemoveConflictingProducts = 1
    End If
End Function

Private Function IsTargetProduct(displayName)
    Dim nameUpper
    nameUpper = UCase(Trim(displayName))
    IsTargetProduct = (nameUpper = "SUNSHINE") _
        Or (nameUpper = "APOLLO") _
        Or (nameUpper = "VIBEPOLLO")
End Function

Private Function ReadStringValue(reg, hive, path, valueName)
    On Error Resume Next
    Dim value, rc
    value = ""
    rc = reg.GetStringValue(hive, path, valueName, value)
    If rc <> 0 Or IsNull(value) Then
        value = ""
        rc = reg.GetExpandedStringValue(hive, path, valueName, value)
    End If
    If IsNull(value) Then
        value = ""
    End If
    ReadStringValue = CStr(value)
End Function

Private Sub LogMessage(message)
    On Error Resume Next
    Session.Log message
End Sub

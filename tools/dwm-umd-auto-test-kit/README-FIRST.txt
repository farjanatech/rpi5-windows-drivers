RPi5 DWM UMD AUTO INSTALL + TEST KIT v1.0
============================================

PURPOSE
-------
One-click deployment and reproduction kit for the Pi5D3D / DWM black-screen investigation.

The graphics payload is the Release ARM64 "DWM UMD First-Fatal Diagnostic v1"
built from Universal Graphics 1.0.0.0. Its KMD/HVS/V3D behavior is unchanged.
Only Pi5D3D.dll has diagnostic first-fatal instrumentation.

WHAT IS AUTOMATIC
-----------------
1. Copies the kit to ProgramData.
2. Removes a leftover truncatememory setting if present.
3. Enables TESTSIGNING if necessary and resumes after reboot.
4. Imports the diagnostic test certificate.
5. Stages and force-binds the diagnostic Graphics INF to ACPI\RPI1001.
6. Preserves the previous INF in the Driver Store for rollback.
7. Reboots into the diagnostic driver.
8. Arms the DWM/driver recorder as SYSTEM.
9. Automatically opens Settings -> Time & language -> Date & time after logon.
10. Captures DWM PID changes, WER, Pi5Display/Pi5Render traces, DBWIN output,
    and C:\ProgramData\Pi5GraphicsDiagnostics first-fatal files.
11. If you reboot after a black screen, it finalizes on the next boot.
12. If you do nothing, it automatically finalizes after 10 minutes.
13. Deletes ONLY the diagnostic published INF, restores TESTSIGNING if this kit
    originally enabled it, and reboots.
14. Verifies that the diagnostic INF is no longer active.
15. Leaves the result ZIP on the Public Desktop.

HOW TO RUN
----------
Extract the whole ZIP to a normal local folder.

Right-click or double-click:
    START-AUTO-DWM-UMD-TEST.cmd

Approve UAC.

After that, let the kit run. It may reboot more than once during preparation.

On the diagnostic test boot, log into Windows normally. The kit automatically
opens Date & time Settings. You may additionally right-click the Desktop/File
Explorer while the display is usable, but this is optional.

If the display becomes black:
- You do NOT need to launch another tester.
- You may reboot/power-cycle if needed.
- If Windows remains alive, the 10-minute timeout should finalize and rollback
  automatically even with the screen unusable.

RESULT
------
After rollback, the Public Desktop should contain:
    RPI5-DWM-UMD-AUTO-RESULT-*.zip
    RPi5-DWM-UMD-AUTO-TEST-COMPLETE.txt

Upload the RESULT ZIP to ChatGPT.

SAFETY / SCOPE
--------------
- Does not change UEFI.
- Does not change HVS registers or display modes beyond the driver itself.
- Does not alter graphics-memory size.
- Removes leftover truncatememory before testing.
- The diagnostic INF is tracked by its exact published OEM INF name.
- Rollback refuses to delete the recorded baseline INF.
- The previous graphics package remains in Driver Store.
- If TESTSIGNING was already enabled before the kit, it remains enabled.
- If the kit enabled TESTSIGNING, rollback turns it back off.

EMERGENCY
---------
If automatic rollback does not complete, boot Windows if necessary and run:
    EMERGENCY-ROLLBACK.cmd

Do not manually delete random 1.0.0.0 graphics packages; the diagnostic and
baseline driver share the same DriverVersion. This kit removes only the exact
diagnostic OEM INF recorded during installation.

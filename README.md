# wslg-sway
Let WSLG use the Sway compositor

---
### Requirements

- Windows with WSL support
- WSL version supporting custom distributions

---
### Install

##### 1. Download the latest VHD package. Example:
```
system.zip
```

##### 2. Extract the downloaded archive. After extraction, you should get:
```
system.vhd
```

##### 3. Open **WSL Settings**.
<img width="478" height="408" alt="image" src="https://github.com/user-attachments/assets/31cbcec6-33a2-4c5f-939f-7508d4fb18c0" />

##### 4. Go to `Developer -> Custom system distro` then select the extracted `system.vhd` file, example:
<img width="1284" height="791" alt="image" src="https://github.com/user-attachments/assets/b5912fb7-8602-40a0-a947-037b5c1040ff" />

##### 5. Enter the system distribution version using a terminal:
```
wsl --system -u wslg
```
<img width="625" height="158" alt="image" src="https://github.com/user-attachments/assets/de7f32bc-dd1d-4711-bd65-e04673b69abc" />

##### 6. Copy the default configuration file:
```
cp /etc/sway/config /mnt/c/Users/[your user name]/swayconfig
```
<img width="1332" height="212" alt="image" src="https://github.com/user-attachments/assets/dcb439e1-f807-4df1-97c1-136214663d63" />


## Configuration
For configuration instructions, please refer to (https://github.com/swaywm/sway/blob/master/config.in)

configuration file path:
```
C:\ProgramData\Microsoft\WSL\swayconfig or C:\Users\[your user name]\swayconfig
```

```
Please configure the shortcut key to open the terminal as follows:
set $term /mnt/c/Windows/System32/wsl.exe -- SWAYSOCK="$SWAYSOCK" foot
bindsym $mod+t exec $term
```

```
Please configure the shortcut keys to switch monitors:
bindsym $mod+f11 next_monitor
```

```
Similar configurations can prevent WSL for auto shutdown:
exec_always /mnt/c/Windows/System32/wsl.exe -- swaybg -c 222222
```

---
### VHD Permission Issue

If there's this error: WsL/Service/CreateInstance/CreateVm/HCS/E_ACCESSDENIED

1. Locate the VHD file.

2. Open file properties.

3. Add the `Users` group.

4. Grant read and execute permissions.

5. Restart WSL and try again.

example:

<img width="363" height="450" alt="image" src="https://github.com/user-attachments/assets/25c1f8ba-a522-4b76-be57-fa5ec156a317" />

---
## References

The project is based on or inspired by:
- freerdp  
  https://github.com/FreeRDP/FreeRDP
- sway  
  https://github.com/swaywm/sway

thank you:
- wslg  
  https://github.com/microsoft/wslg

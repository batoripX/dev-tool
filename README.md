# dev-tool

dev-tool is a developer cli tool in modern c++ works good and the info is provided once you download the prebuilt windows exe so if you wanna know command arguments the exe already provides that and this is strictly a windows tool there is no guarantees that there will be a linux version unless i decide to add support for that.

Download `dev.exe` from Releases, open PowerShell in its folder, and add it to your `PATH`:

```powershell
[Environment]::SetEnvironmentVariable("Path", [Environment]::GetEnvironmentVariable("Path","User") + ";\$PWD", "User")
```

Restart your terminal and run `dev` from anywhere. Note: this command is only for PowerShell (Windows PowerShell 5.1 / PowerShell 7+)—no guarantees it will work in Git Bash or other shells since it uses a Windows .NET API. Enjoy the exe :)

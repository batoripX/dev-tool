# dev-tool

dev-tool is a developer cli tool in modern c++ works good and the info is provided once you download the prebuilt windows exe so if you wanna know command argument the exe already provides that and this is strictly a windows tool

Download `dev.exe` from Releases, open PowerShell in its folder, and add it to your `PATH`:

```powershell
\$p=[Environment]::GetEnvironmentVariable("Path","User");if(p -notlike "*PWD*"){[Environment]::SetEnvironmentVariable("Path","p;PWD","User")}
```

Restart your terminal and run `dev` from anywhere. Note: this command is only for PowerShell (Windows PowerShell 5.1 / PowerShell 7+)—no guarantees it will work in Git Bash or other shells since it uses a Windows .NET API. Enjoy the exe :)

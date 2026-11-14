# dev-tool
dev-tool is a developer cli tool in modern c++ works good and the info is provided once you download the prebuilt windows exe.

Download `dev.exe` from Releases, open PowerShell in its folder, and add it to your `PATH`:

```powershell
\$p=[Environment]::GetEnvironmentVariable("Path","User");if(p -notlike "*PWD*"){[Environment]::SetEnvironmentVariable("Path","p;PWD","User")}
```

Restart your terminal and run `dev` from anywhere.
note btw this is only for powershell 7 i have no guarantees or certain that it will work on bash or other terminals as this is a strictly NET and a windows commmand enjoy the exe :)

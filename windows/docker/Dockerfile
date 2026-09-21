# Windows scenario image: VS Build Tools (C++ workload) on Server Core.
# Windows containers require a Windows host with Containers enabled and an
# image tag matching the host OS build (adjust ltsc2022 to your host).
#
#   docker build -f windows/docker/Dockerfile -t interpose:windows-2022-x64 .
#   docker run --rm -v C:\path\to\repo:C:\src -w C:\src interpose:windows-2022-x64 ^
#       cmd /c windows\tests\run_scenario.cmd
FROM mcr.microsoft.com/windows/servercore:ltsc2022

SHELL ["powershell", "-Command", "$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue';"]

# cl/link/dumpbin + vcvars64; vswhere ships with the installer, which
# windows\msvc-env.cmd uses to locate the toolchain.
# Notes, learned the hard way on servercore ltsc2022:
# - separate RUN steps, one command each: Dockerfile line continuations are
#   backslashes, not PowerShell backticks
# - the installer must run via Start-Process; invoked directly with `&` it
#   exits 1 in containers. On failure the installer's dd_*.log tails are
#   printed to the build log for diagnosis.
# - WORKDIR needs forward slashes: the Dockerfile parser eats the backslash
#   in `C:\src` and yields the invalid `C:src`
RUN Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile C:\vs_buildtools.exe
RUN $p = Start-Process -Wait -PassThru -FilePath C:\vs_buildtools.exe -ArgumentList '--quiet --norestart --nocache --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'; Write-Output ('bootstrapper exit: ' + $p.ExitCode); if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Get-ChildItem ($env:TEMP + '\dd_*.log'), ($env:TEMP + '\*\dd_*.log') -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ('===== ' + $_.FullName); Get-Content $_ -Tail 50 }; exit 1 }
RUN Remove-Item C:\vs_buildtools.exe

WORKDIR C:/src

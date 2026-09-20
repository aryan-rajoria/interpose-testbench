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
RUN Invoke-WebRequest `
        -Uri 'https://aka.ms/vs/17/release/vs_buildtools.exe' `
        -OutFile C:\vs_buildtools.exe; `
    Start-Process -Wait -FilePath C:\vs_buildtools.exe -ArgumentList `
        '--quiet','--norestart','--nocache','--wait',`
        '--add','Microsoft.VisualStudio.Workload.VCTools',`
        '--includeRecommended'; `
    Remove-Item C:\vs_buildtools.exe

WORKDIR C:\src

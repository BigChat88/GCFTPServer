# Builds gcftpserver.dol inside the libogc2 Docker image.
# Usage: .\build.ps1          (build)
#        .\build.ps1 clean    (clean)
param([string]$Target = "")

# Pinned to the image with libogc2 commit f8f62e1, which netstack/ was taken
# from (it uses libogc2 internal headers). Keep in sync with
# .github/workflows/build.yml.
$Image = "ghcr.io/extremscorner/libogc2@sha256:95440883dc298ea316dd46edfc0b1af10af63c6492b1f52b35081f47320ca635"

docker run --rm -v "${PSScriptRoot}:/project" -w /project $Image make $Target

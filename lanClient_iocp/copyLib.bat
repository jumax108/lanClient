
mkdir lanClient

robocopy headers lanClient/headers
robocopy release lanClient *.pdb
robocopy release lanClient *.lib

pause
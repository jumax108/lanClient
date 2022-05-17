
set name="lanClient"

mkdir %name%

robocopy headers %name%/headers
robocopy release %name% *.pdb
robocopy release %name% *.lib

erase release /S /Q
rd release /S /Q

pause
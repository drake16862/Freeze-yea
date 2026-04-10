Freeze OS import folder

Any regular file in this folder is copied to /home in the ext2 image
when you run make.

Local import:
	cp my_notes.txt FreezeProject/import/
	make run

Online import:
	make import URL=https://example.com/file.txt
	make run

Optional custom name:
	make import URL=https://example.com/file.txt NAME=notes.txt

Inside Freeze OS:
	ls /home
	cat /home/notes.txt

Notes:
	- Imports happen on the host during build time.
	- Only top-level regular files from FreezeProject/import/ are imported.

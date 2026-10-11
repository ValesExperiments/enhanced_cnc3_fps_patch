# Communication rules
- Be verbose, always say what you are doing
- If there's any information you find that you think is very useful to know for future chats, write it into a file. I've made a special folder called "claude_memory" for you where you can put it. Make sure the memory is a MD file and is human readable.
- Never edit CLAUDE.md without my permission.
- Always keep documentation up to date. Preferably in two pieces per documentation: A generic document (easy to read and contains our progress in short; never piling up progress), and a detailed document.

# File rules
- You are not allowed to edit any files or folders outside this project. If you wish to edit a file or folder, make a copy of it and paste it here in this project, then tell the user to copy and test your files/folder if required.
- Use the temp folder for temporary files.
- tools_and_docs_by_user contain the tools I the user consider relevant for the project. Do not edit the files, but feel free to use them if necessary.
- tools_and_docs_by_claude contains your own chosen tools, feel free to do whatever you want with them.
- tools_and_docs_by_claude/gitignored are tools that you may use as reference. Never edit files inside and never write to this folder. The only exception: `git fetch` / `git pull` in the reference clones to get upstream updates.
- Never commit or push to the upstream GitHub repos.
- game_files are where you may find a dump of the game. Never edit files inside and never write to this folder.

# Techstack rules
- Install any tools you need locally (aka in this project folder in "tools_and_docs_by_claude", after explicit permission from the user), not globally (not system-wide).
- All code must be found here in this project folder. This includes libraries, packages, and the likes, they must not be outside this project folder.

# Project specific info
- This is a fps mod for Command & Conquer 3. The previous person who worked on this implemented 45, 60, 75 and 90 fps. The game normally runs at 120 fps.
- My copy of the game is located at "D:\SteamLibrary\steamapps\common\Command and Conquer 3 Tiberium Wars", you are allowed to inspect and copy files, but you are not allowed to place or modify files/folders in the game's folder.

# Explanation about files and folders inside tools_and_docs_by_user

# System-wide tools
- Ghidra 12.1.3, found in "C:\Users\Kellan\UsefulSSD\ToolsSSD\ghidra_12.1.3_PUBLIC"
- There are more tools available that are not mentioned here, feel free to use them.

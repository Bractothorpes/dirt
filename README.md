# Dirt - [Dir]ectory [t]ree
## A simple terminal file viewer that lets you browse, open, and search files easily.

---

### Installation

**Step 1:** Head to the latest release [here](https://github.com/Bractothorpes/dirt/releases/tag/v0.2.0) and download the `dirt-v0.2.0.zip` file.  
**Step 2:** Extract the contents of the zip file.  
**Step 3:** Run `dirtinstaller.exe` **with admin permissions** to add Dirt to your system PATH.  
**Step 4:** *(Optional)* Delete the original Dirt folder from your Downloads folder.  
**Step 5:** Enjoy! You can now use Dirt anywhere - try it out by typing `dirt` in your terminal.

---

### How to Use

#### **Navigation**
- Use the **arrow keys** to move through files and folders.  
- Press **Enter** to open a file or expand/collapse a folder.  
- The same controls also work inside the *Find* menu.

#### **Keybinds**
- **q** → Quit Dirt or exit the Find menu  
- **f** → Search through all non-binary files for a piece of text  
- **r** → Refresh (useful if you’ve added new files)  
- **g** → Jump to the top  
- **G** → Jump to the bottom  

---

### Configuration

After installation:
- **Windows:** `C:\Program Files\dirt`  
- **Linux / macOS:** `/usr/local/dirt`

Inside that directory is a file named **`.dirtconfig`**.  
You can edit it in two ways:

1. **Changing the application used to run based on extension, and default:**
  ```ini
  editor_generic=nvim
  .txt=notepad
  .py=code
```
2. **Change the ignored word search folders:**
  ```ini
  skip_dirs=.git,node_modules,.cache,build,dist
```

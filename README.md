## Features
- Raw text editor
- Scrolling (with keyboard & mouse)
- File I/O
- Status bar with line/column number
- Searching
- Syntax highlight
- Cursor position snapping and memorization

## Setup
You will need a C compiler.  

### Linux:  
```
sudo apt-get install gcc make
```  
### Windows (Bash):  
```
sudo apt-get install gcc make
```
### macOS
```
xcode-select --install
```

## Compile and run only
```
make
```
```
./mte YOUR_FILE
./mte test.txt  #example
```

## Install and run
```
sudo make install
```
```
mte YOUR_FILE
mte mte.c  #example
```

## Upcoming features
- Copy and paste
- Undo & Redo
- Auto indent
- Line warp
- Configurable settings
- Additional filetype support with custom imports
- Replace array buffer with Rope data structure
- Vim like mode switching (Normal/Insert mode)

## Reference materials:
- http://antirez.com/news/108
- https://github.com/antirez/kilo
- https://austinhenley.com/blog/challengingprojects.html
- https://www.averylaird.com/programming/the%20text%20editor/2017/09/30/the-piece-table
- https://viewsourcecode.org/snaptoken/kilo/index.html
- https://en.wikipedia.org/wiki/Memento_pattern

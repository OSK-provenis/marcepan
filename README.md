# marcepan

A multi-threaded Mandelbrot and Julia set viewer for Linux terminals. Renders fractals as ASCII art with ANSI 256-color support.

## What it does

marcepan visualizes the Mandelbrot and Julia fractals in your terminal using ASCII characters and colors. It separates calculation from presentation, allowing instant palette switching without recalculation. Optional half-block mode using Unicode characters doubles the vertical resolution.

![Screenshot](images/marcepan_screenshot002.png)

## Compilation

Linux only. Requires GCC and POSIX threads:

```bash
gcc -O3 -std=c11 -pthread -o marcepan marcepan.c -lm
```

## Features

- **Multi-threaded rendering** - Uses all CPU cores by default
- **Mandelbrot and Julia sets** - Switch between them with a keypress
- **16 built-in ASCII palettes** - Plus custom palette support
- **16 color schemes** - ANSI 256-color palettes
- **Half-block mode** - 2x vertical resolution using ▀▄ Unicode characters
- **Two mapping modes** - Modulo (banded) or linear (smooth gradient)
- **Export capabilities** - Save as plain .txt or colored .ansi files
- **Interactive navigation** - Numpad controls for easy exploration
- **Copy-paste commands** - Header shows command to recreate current view

## Usage

```bash
# Default settings
./marcepan

# Start with Julia set
./marcepan -j -0.7 0.27015

# High iteration depth with specific palette
./marcepan -i 200 -pal 4 -col 8

# Half-block mode for better resolution
./marcepan -hb

# Custom ASCII palette
./marcepan --symbols " .:+*#@"

# Batch mode (render once and exit)
./marcepan -b -x -0.5 0.0 -y -0.5 0.5 -i 100
```

## Command-line Options

| Option | Description |
|--------|-------------|
| `-t N` | Number of worker threads (default: auto-detect) |
| `-nc` | Disable color output |
| `-x MIN MAX` | X-axis range (default: -2.0 1.0) |
| `-y MIN MAX` | Y-axis range (default: -1.0 1.0) |
| `-i N` | Max iterations (default: 30, max 10000) |
| `-pal N` | ASCII palette 1-16 (default: 2) |
| `-col N` | Color scheme 1-16 (default: 1) |
| `-m, --mode M` | Mapping mode: `mod` (default) or `lin` |
| `-j CR CI` | Julia mode with constant c = CR + CI*i |
| `-hb` | Enable half-block mode (2x vertical resolution) |
| `--symbols "S"` | Custom ASCII palette (2-256 characters) |
| `-b, --batch` | Render once and exit (non-interactive) |
| `-h, --help` | Show help message |

## Keyboard Controls

**Note:** NumLock must be OFF for numpad keys to work.

### Navigation

| Key | Action |
|-----|--------|
| **Numpad 8/2/4/6** | Pan up/down/left/right |
| **Numpad 7/9/1/3** | Pan diagonally |
| **Numpad 0** (Ins) | Zoom in |
| **Numpad Enter** | Zoom out |
| **Shift + Arrows** | Stretch/shrink individual axes |

### Adjustments

| Key | Action |
|-----|--------|
| **+** / **-** | Increase/decrease iteration depth |
| **/** / **\*** | Cycle through ASCII palettes |
| **1** / **2** | Cycle through color schemes |

### Toggles

| Key | Action |
|-----|--------|
| **c** | Toggle color on/off |
| **m** | Toggle modulo/linear mapping mode |
| **j** | Toggle Julia/Mandelbrot mode |
| **h** | Toggle half-block rendering |

### Other

| Key | Action |
|-----|--------|
| **p** | Save to .txt file (plain ASCII) |
| **P** (Shift+p) | Save to .ansi file (with colors) |
| **ESC** | Reset to default view |
| **q** | Quit |

## Notes

- The header shows a command that can be copy-pasted to recreate the current view
- In Julia mode, pressing `j` while viewing the Mandelbrot set will use the center point as the Julia constant c
- Modulo mode creates repeating color bands (classic look), linear mode creates smooth gradients
- Half-block mode uses `▀` and `▄` characters to achieve 2x vertical resolution
- Files are saved with timestamp: `marcepan_YYYYMMDD_HHMMSS.txt` or `.ansi`

## License

This is free and unencumbered software released into the public domain.

See [The Unlicense](https://unlicense.org/) for details.

Enjoy your marcepan!

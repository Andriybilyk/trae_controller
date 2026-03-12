# Cyrillic Smooth Fonts for Trae Controller

To enable smooth Cyrillic fonts (anti-aliased) on the display, please follow these steps:

1.  **Download a Font**: Find a `.ttf` font that supports Cyrillic characters (e.g., `OpenSans-Regular.ttf`, `Roboto-Regular.ttf`, or `DejaVuSans.ttf`). You can download these from [Google Fonts](https://fonts.google.com/).

2.  **Generate a VLW Font (Recommended)**:
    *   Download and install [Processing IDE](https://processing.org/download/).
    *   Download the `Create_VLW_Font` tool from the [TFT_eSPI repository](https://github.com/Bodmer/TFT_eSPI/tree/master/Tools/Processing/Create_VLW_Font).
    *   Open the `Create_VLW_Font.pde` sketch in Processing.
    *   Run the sketch. It will list installed fonts. Select your Cyrillic font.
    *   Make sure to include the Cyrillic unicode range (U+0400 to U+04FF).
    *   Save the generated `.vlw` file as `font.vlw`.

3.  **Place the File**:
    *   Copy the generated `font.vlw` file into this directory: `firmware/data/fonts/font.vlw`.

4.  **Upload Filesystem**:
    *   Use PlatformIO's "Upload Filesystem Image" task to upload the `data` folder to the ESP32.

The firmware is configured to look for `/fonts/font.vlw`. If found, it will use it for smooth text rendering. If not found, it will fall back to the built-in pixelated font.

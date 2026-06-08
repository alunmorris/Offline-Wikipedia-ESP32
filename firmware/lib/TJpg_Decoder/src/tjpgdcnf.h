/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/

#define	JD_SZBUF		512
/* Specifies size of stream input buffer */

#define JD_FORMAT		0
/* Specifies output pixel format.
/  0: RGB888 (24-bit/pix)
/  1: RGB565 (16-bit/pix)
/  2: Grayscale (8-bit/pix)
*/

#define	JD_USE_SCALE	1
/* Switches output descaling feature.
/  0: Disable
/  1: Enable
*/

#define JD_TBLCLIP		0
/* Use table conversion for saturation arithmetic. A bit faster, but increases 1 KB of code size.
/  0: Disable
/  1: Enable
*/

#define JD_FASTDECODE	1
/* Optimization level
/  0: Basic optimization. Suitable for 8/16-bit MCUs.
/     Workspace of 3100 bytes needed.
/  1: + 32-bit barrel shifter. Suitable for 32-bit MCUs.
/     Workspace of 3480 bytes needed.
/  2: + Table conversion for huffman decoding (wants 6 << HUFF_BIT bytes of RAM).
/     Workspace of 9644 bytes needed.
*/

// Workspace size increased for JD_FORMAT=0 (RGB888): workbuf uses n*64*3
// instead of n*64*2 for 4:2:0 MCUs (+256 bytes), so add ≥256 + margin.
#if JD_FASTDECODE == 0
 #define TJPGD_WORKSPACE_SIZE 3500
#elif JD_FASTDECODE == 1
 #define TJPGD_WORKSPACE_SIZE 4096
#elif JD_FASTDECODE == 2
 #define TJPGD_WORKSPACE_SIZE (4096 + 6144)
#endif
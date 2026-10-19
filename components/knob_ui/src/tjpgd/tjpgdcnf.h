/*----------------------------------------------*/
/* TJpgDec System Configurations (standalone)   */
/*----------------------------------------------*/

#define JD_SZBUF 512

// RGB888, so art_decoder can dither down to RGB565 instead of truncating.
#define JD_FORMAT 0
/* 0: RGB888 (24-bit/pix)
/  1: RGB565 (16-bit/pix)
/  2: Grayscale (8-bit/pix)
*/

#define JD_USE_SCALE 1

#define JD_TBLCLIP 1

#define JD_FASTDECODE 2

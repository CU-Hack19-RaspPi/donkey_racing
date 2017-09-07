#if !defined(table_h)
#define table_h

/* generated by maketable.py */

#define SOURCE_WIDTH 640
#define SOURCE_HEIGHT 480
#define INPUT_LEFT 96
#define INPUT_TOP 240
#define INPUT_WIDTH 448
#define INPUT_HEIGHT 177
#define RECTIFIED_WIDTH 149
#define RECTIFIED_HEIGHT 59

typedef struct TableInputCoord {
  float sy;
  float sx;
} TableInputCoord;

/* total table size 8791 elements 70328 bytes */
extern TableInputCoord const sTableInputCoords[RECTIFIED_HEIGHT][RECTIFIED_WIDTH];

#endif
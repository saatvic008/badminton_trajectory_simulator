#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <string.h>
#include <windows.h>  // Sleep(), system("cls")

/*
 topview_badminton.c
 Top-view ASCII badminton court with shuttle flying animation (pure C).
 - Computes 3D trajectory (forward y, lateral x, height z)
 - Animates top view (y vs lateral x) on terminal ASCII court
 - Reports landing, net-clear status (uses z when crossing net)
 Compile/run: build in Visual Studio or with gcc.
*/

// ------------------- CONFIG / PHYSICS -------------------
#define COURT_LENGTH 13.40      // meters
#define COURT_WIDTH  5.18       // singles width (meters)
#define NET_POS (COURT_LENGTH/2.0)
#define NET_HEIGHT 1.524        // meters (used to check clearance)
#define GRAVITY 9.81
#define DT 0.01                 // integration timestep (s)
#define DRAG 0.018              // linear drag coef (tuneable)
#define MAX_STEPS 20000
#define TIMEOUT 8.0             // seconds safety stop

// ------------------- TERMINAL LAYOUT -------------------
// Grid area used to draw court (top-view)
#define GRID_COLS 72    // horizontal resolution (maps to court length)
#define GRID_ROWS 21    // vertical resolution (maps to lateral width)
#define PANEL_W (GRID_COLS + 2)
#define PANEL_H (GRID_ROWS + 4)

// characters
#define CHAR_PLAYER 'P'
#define CHAR_SHUTTLE 'O'
#define CHAR_LANDING 'X'
#define CHAR_TRAIL '.'
#define CHAR_SHADOW '_'

// map world meters -> grid coords
static int mapYtoCol(double y) {
    if (y < 0) y = 0; if (y > COURT_LENGTH) y = COURT_LENGTH;
    return (int)round((y / COURT_LENGTH) * (GRID_COLS - 1));
}
static int mapXtoRow(double xlat) {
    // lateral xlat: center 0.0; range [-COURT_WIDTH/2 .. +COURT_WIDTH/2]
    double half = COURT_WIDTH / 2.0;
    if (xlat < -half) xlat = -half;
    if (xlat > half) xlat = half;
    // map left (-half) -> row 0, right (+half) -> row GRID_ROWS-1
    double frac = (xlat + half) / (COURT_WIDTH);
    return (int)round(frac * (GRID_ROWS - 1));
}

// posture contact height (meters)
static double contact_height(double player_height, int posture) {
    double base = 0.85 * player_height;
    if (posture == 1) return base - 0.30; // bent
    if (posture == 2) return base + 0.42; // in air
    return base; // standing
}

// shot params
static void shot_params(int shotType, double *angle_deg, double *speed_mult, double *lat_spread_deg) {
    switch(shotType) {
        case 0: // smash
            *angle_deg = -10.0; *speed_mult = 1.5; *lat_spread_deg = 2.5; break;
        case 1: // clear
            *angle_deg = 40.0; *speed_mult = 0.95; *lat_spread_deg = 6.0; break;
        case 2: // drop
            *angle_deg = 12.0; *speed_mult = 0.7; *lat_spread_deg = 10.0; break;
        case 3: // drive
        default:
            *angle_deg = 2.0; *speed_mult = 1.2; *lat_spread_deg = 6.0; break;
    }
}

// clamp helper
static double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// clear console (windows)
static void cls() { system("cls"); }

// draw top-view court into char buffer
static void draw_court(char buf[PANEL_H][PANEL_W]) {
    // fill spaces
    for (int r=0;r<PANEL_H;r++) for (int c=0;c<PANEL_W;c++) buf[r][c] = ' ';
    // draw border
    for (int c=0;c<PANEL_W-2;c++) { buf[1][c+1] = '-'; buf[PANEL_H-2][c+1] = '-'; }
    for (int r=1;r<PANEL_H-1;r++) { buf[r][0] = '|'; buf[r][PANEL_W-2] = '|'; }
    buf[1][0] = buf[1][PANEL_W-2] = '+'; buf[PANEL_H-2][0] = buf[PANEL_H-2][PANEL_W-2] = '+';

    // inner court area mapped to GRID_ROWS x GRID_COLS inside border
    int off_r = 2; int off_c = 1;
    // draw sidelines (top/bottom of grid area)
    for (int c=0;c<GRID_COLS;c++) {
        buf[off_r + GRID_ROWS][off_c + c] = '-'; // bottom line
        buf[off_r - 1][off_c + c] = '-'; // top cap for readability
    }
    // draw center line (vertical at net)
    int net_col = mapYtoCol(NET_POS);
    for (int r=0;r<GRID_ROWS;r++) {
        buf[off_r + r][off_c + net_col] = '|';
    }
    // draw service lines (approx positions): singles service line is at ~1.98m from net each side,
    // but top-view we mark lines roughly at y positions:
    double svc_front_dist = 1.98; // not exact, illustrative
    int svc_left_col = mapYtoCol(NET_POS - svc_front_dist);
    int svc_right_col = mapYtoCol(NET_POS + svc_front_dist);
    for (int r=0;r<GRID_ROWS;r++) {
        // short markers to indicate service area (small gaps)
        buf[off_r + r][off_c + svc_left_col] = '+';
        buf[off_r + r][off_c + svc_right_col] = '+';
    }
    // draw center horizontal line (mid-lateral)
    int mid_row = off_r + GRID_ROWS/2;
    for (int c=0;c<GRID_COLS;c++) {
        char prev = buf[mid_row][off_c + c];
        if (prev == ' ') buf[mid_row][off_c + c] = '-';
    }
    // add distance labels below
    char lbl[80];
    sprintf(lbl, "0m");
    int c0 = off_c + mapYtoCol(0);
    for (int i=0;i< (int)strlen(lbl); i++) if (c0+i < PANEL_W-2) buf[PANEL_H-1][c0+i] = lbl[i];
    sprintf(lbl, "NET");
    int cn = off_c + mapYtoCol(NET_POS) - 1;
    for (int i=0;i< (int)strlen(lbl); i++) if (cn+i < PANEL_W-2) buf[PANEL_H-1][cn+i] = lbl[i];
    sprintf(lbl, "13.4m");
    int ce = off_c + mapYtoCol(COURT_LENGTH) - 5;
    for (int i=0;i< (int)strlen(lbl); i++) if (ce+i < PANEL_W-2) buf[PANEL_H-1][ce+i] = lbl[i];
}

// print buffer to screen
static void print_buf(char buf[PANEL_H][PANEL_W]) {
    for (int r=0;r<PANEL_H;r++) {
        for (int c=0;c<PANEL_W-1;c++) putchar(buf[r][c]);
        putchar('\n');
    }
}

// main simulation: compute 3D trajectory and store points
typedef struct { double y; double xlat; double z; } Point3;
static int simulate_trajectory(
    double player_height,
    int posture,
    double swing_speed,
    double tension_mult,     // map tension into multiplier approx (1.0 default)
    double contact_offset,   // meters from sweet spot (0 best)
    int shotType,
    double yaw_deg,          // lateral aiming (deg)
    Point3 traj[], int max_points,
    double *out_landing_y, double *out_landing_x, double *out_landing_z,
    int *out_cleared_net
) {
    // initial contact height
    double z = contact_height(player_height, posture);
    double y = 0.0;
    double xlat = 0.0;

    double angle_deg, speed_mult, lat_spread;
    shot_params(shotType, &angle_deg, &speed_mult, &lat_spread);

    // contact offset reduces speed slightly
    double contact_mult = 1.0 - clampd(contact_offset / 0.03, 0.0, 0.4);
    double v0 = swing_speed * speed_mult * tension_mult * contact_mult;
    if (v0 < 2.0) v0 = 2.0;

    // combine elevation and yaw
    double elev = angle_deg * M_PI / 180.0;
    double yaw = yaw_deg * M_PI / 180.0;

    // initial velocity components
    double vy = v0 * cos(elev) * cos(yaw);
    double vx = v0 * cos(elev) * sin(yaw);
    double vz = v0 * sin(elev);

    int idx = 0;
    double t = 0.0;
    int net_checked = 0; int cleared = 0;

    while (t < TIMEOUT && idx < max_points) {
        // store
        traj[idx].y = y; traj[idx].xlat = xlat; traj[idx].z = z;
        idx++;

        // net check when crossing forward
        if (!net_checked && y >= NET_POS) {
            net_checked = 1;
            if (z > NET_HEIGHT + 0.02) cleared = 1;
            else cleared = 0;
        }

        // update accelerations with simple linear drag
        double ay = -DRAG * vy;
        double ax = -DRAG * vx;
        double az = -GRAVITY - DRAG * vz;

        // integrate semi-implicit Euler
        vy += ay * DT;
        vx += ax * DT;
        vz += az * DT;

        y += vy * DT;
        xlat += vx * DT;
        z += vz * DT;

        t += DT;

        // landing condition
        if (z <= 0.0) {
            // record final landing point
            *out_landing_y = y;
            *out_landing_x = xlat;
            *out_landing_z = 0.0;
            *out_cleared_net = cleared;
            return idx;
        }

        // bounds: if shot goes very far outside we stop
        if (y > COURT_LENGTH + 8.0 || fabs(xlat) > COURT_WIDTH + 5.0) break;
    }

    // if timeout or bounds reached, report last pos as landing (may be mid-air)
    *out_landing_y = y;
    *out_landing_x = xlat;
    *out_landing_z = z;
    *out_cleared_net = net_checked ? cleared : 0;
    return idx;
}

// animate on terminal top-view using stored trajectory
static void animate_topview(Point3 traj[], int npoints, double land_y, double land_x, int cleared_net) {
    char buf[PANEL_H][PANEL_W];
    draw_court(buf);

    // place player marker near left baseline center
    int player_col = mapYtoCol(0);
    int player_row = mapXtoRow(0.0);
    int off_r = 2; int off_c = 1;
    buf[off_r + player_row][off_c + player_col] = CHAR_PLAYER;

    // trail map: mark previous positions
    char trail[GRID_ROWS][GRID_COLS];
    for (int r=0;r<GRID_ROWS;r++) for (int c=0;c<GRID_COLS;c++) trail[r][c] = ' ';

    int landing_col = mapYtoCol(land_y);
    int landing_row = mapXtoRow(land_x);

    // animate frames
    for (int i=0;i<npoints;i++) {
        // update trail
        int c = mapYtoCol(traj[i].y);
        int r = mapXtoRow(traj[i].xlat);
        if (r>=0 && r<GRID_ROWS && c>=0 && c<GRID_COLS) trail[r][c] = CHAR_TRAIL;

        // refresh base court
        draw_court(buf);
        // player
        buf[off_r + player_row][off_c + player_col] = CHAR_PLAYER;
        // draw trail onto buf
        for (int rr=0; rr<GRID_ROWS; rr++) {
            for (int cc=0; cc<GRID_COLS; cc++) {
                char ch = trail[rr][cc];
                if (ch != ' ') buf[off_r + rr][off_c + cc] = ch;
            }
        }
        // shuttle current pos
        int sc = mapYtoCol(traj[i].y);
        int sr = mapXtoRow(traj[i].xlat);
        if (sr>=0 && sr<GRID_ROWS && sc>=0 && sc<GRID_COLS) buf[off_r + sr][off_c + sc] = CHAR_SHUTTLE;

        // shadow on ground row (place underscore below shuttle near bottom)
        int shadow_row = off_r + GRID_ROWS; // below grid
        if (shadow_row < PANEL_H-1 && sc>=0 && sc<GRID_COLS) buf[shadow_row][off_c + sc] = CHAR_SHADOW;

        // landing marker (if near or after)
        if (i == npoints-1 || (fabs(traj[i].y - land_y) < 0.02 && fabs(traj[i].xlat - land_x) < 0.02)) {
            if (landing_row>=0 && landing_row<GRID_ROWS && landing_col>=0 && landing_col<GRID_COLS)
                buf[off_r + landing_row][off_c + landing_col] = CHAR_LANDING;
        }

        // clear & print
        cls();
        print_buf(buf);

        // status text
        printf("\nSimulating shot... t=%.2fs  pos y=%.2fm xlat=%.2fm z=%.2fm\n",
               i * DT, traj[i].y, traj[i].xlat, traj[i].z);
        Sleep(35); // animation speed (~35 ms per frame)
    }

    // final summary
    printf("\nRESULT: Landing at y=%.2f m  xlat=%.2f m   Net cleared: %s\n",
           land_y, land_x, cleared_net ? "YES" : "NO");
    if (land_y < NET_POS) printf(" -> Fell short (before net)\n");
    else if (land_y > COURT_LENGTH) printf(" -> Long (beyond baseline)\n");
    else printf(" -> Landed inside court area (approx)\n");
}

// ------------------- MAIN -------------------
int main(void) {
    printf("Top-view Badminton Simulator (ASCII)\n");
    printf("Enter player height in meters (e.g., 1.75): ");
    double player_h; if (scanf("%lf", &player_h) != 1) return 0;

    printf("Posture: 0=standing,1=bent,2=in_air : "); int posture; if (scanf("%d",&posture)!=1) return 0;
    printf("Swing speed (m/s) (e.g., 24): "); double swing; if (scanf("%lf",&swing)!=1) return 0;
    printf("Tension multiplier (1.0 default, e.g., 1.02): "); double tension; if (scanf("%lf",&tension)!=1) return 0;
    printf("Contact offset from sweetspot (m) (0.0 best, e.g., 0.01): "); double offset; if (scanf("%lf",&offset)!=1) return 0;
    printf("Shot type: 0=smash,1=clear,2=drop,3=drive : "); int shot; if (scanf("%d",&shot)!=1) return 0;
    printf("Yaw (lateral aim in degrees, negative=left, positive=right, e.g., 0): "); double yaw; if (scanf("%lf",&yaw)!=1) return 0;

    // simulate trajectory
    Point3 traj[MAX_STEPS];
    double land_y, land_x, land_z; int cleared_net;
    int n = simulate_trajectory(player_h, posture, swing, tension, offset, shot, yaw,
                                traj, MAX_STEPS, &land_y, &land_x, &land_z, &cleared_net);

    if (n <= 0) {
        printf("Simulation produced no trajectory points.\n");
        return 0;
    }

    // animate top view using computed points
    animate_topview(traj, n, land_y, land_x, cleared_net);

    printf("\nPress Enter to exit...");
    getchar(); getchar();
    return 0;
}

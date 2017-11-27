//
// Created by Giorgio Tresoldi on 03.04.17.
//
#include<stdio.h>
#include<string.h>    //strlen
#include<sys/socket.h>
#include<arpa/inet.h> //inet_addr
#include<unistd.h>    //write
#include<pthread.h>

#include "dump868.h"
#include "nrf905_demod.c"
#include "net_io.h"
#include "util.h"


/* Run Periodic network functions every 5 seconds */
void *threadproc(void *arg)
{
    while(true)
    {
        modesNetPeriodicWork();
        sleep(5);
    }
    return 0;
}


void showHelp(void) {
    printf(
            "-----------------------------------------------------------------------------\n"
                    "| dump868 FLARM Dump     %45s |\n"
                    "-----------------------------------------------------------------------------\n"
                    "--device-index <index>   Select RTL device (default: 0)\n"
                    "--gain <db>              Set gain (default: max gain. Use -10 for auto-gain)\n"
                    "--ppm <error>            Set receiver error in parts per million (default 0)\n"
                    "--enable-rtlsdr-biast    Set bias tee supply on (default off)\n"
                    "--net-port <ports>       TCP Beast output listen ports (default: 30006)\n"


    );
}

//
// =============================== Initialization ===========================
//
void modesInitConfig(void) {
    // Default everything to zero/NULL
    memset(&DumpFLARM, 0, sizeof(DumpFLARM));

    // Now initialise things that should not be 0/NULL to their defaults
    //DumpFLARM.gain                    = MODES_MAX_GAIN;
    //DumpFLARM.freq                    = MODES_DEFAULT_FREQ;
    //DumpFLARM.ppm_error               = MODES_DEFAULT_PPM;
    DumpFLARM.check_crc               = 1;
    DumpFLARM.net_heartbeat_interval  = MODES_NET_HEARTBEAT_INTERVAL;
    DumpFLARM.net_input_raw_ports     = strdup("30001");
    DumpFLARM.net_output_raw_ports    = strdup("30002");
    DumpFLARM.net_output_sbs_ports    = strdup("30003");
    DumpFLARM.net_input_beast_ports   = strdup("30004,30104");
    DumpFLARM.net_output_beast_ports  = strdup("30006");
#ifdef ENABLE_WEBSERVER
    DumpFLARM.net_http_ports          = strdup("8080");
#endif
    //DumpFLARM.interactive_rows        = getTermRows();
    //DumpFLARM.interactive_display_ttl = MODES_INTERACTIVE_DISPLAY_TTL;
    DumpFLARM.html_dir                = HTMLPATH;
    DumpFLARM.json_interval           = 1000;
    DumpFLARM.json_location_accuracy  = 1;
    DumpFLARM.maxRange                = 1852 * 300; // 300NM default max range
}


/* Subroutine: main()
 * Description: get chunks of data from STDIN and forward to sliding_dft()
 * Input:
 *  argc: argument counter
 *  argv: array of arguments
 * Output: exit code
 */
int main(int argc, char **argv) {
    int j;
    uint16_t i;
    size_t len;
    uint8_t raw_buffer[buffer_size * 2]; // each I/Q sample has two bytes!
    int8_t param;

    packet_bytes=29;

    modesInitConfig();


    for (j = 1; j < argc; j++) {
        int more = j+1 < argc; // There are more arguments


        if (!strcmp(argv[j],"--device-index") && more) {
            DumpFLARM.dev_name = strdup(argv[++j]);
        } else if (!strcmp(argv[j],"--gain") && more) {
            DumpFLARM.gain = (int) (atof(argv[++j])); 
        } else if (!strcmp(argv[j],"--help")) {
            showHelp();
            exit(0);
        } else if (!strcmp(argv[j],"--ppm") && more) {
            DumpFLARM.ppm_error = atoi(argv[++j]);
        } else if (!strcmp(argv[j], "--enable-rtlsdr-biast")) {
            DumpFLARM.enable_rtlsdr_biast = 1;
        } else if (!strcmp(argv[j],"--net-port") && more) {
            free(DumpFLARM.net_output_beast_ports);
            DumpFLARM.net_output_beast_ports = strdup(argv[++j]);
        }else if (!strcmp(argv[j],"--other") && more) {
            DumpFLARM.other_options = strdup(argv[++j]);
        } else {
            fprintf(stderr,
                    "Unknown or not enough arguments for option '%s'.\n\n",
                    argv[j]);
            showHelp();
            exit(1);
        }
    }

    /*
     * Networking
     *
     * */

    modesInitNet();

    /* Create Thread for periodic net operations */
    pthread_t tid;
    pthread_create(&tid, NULL, &threadproc, NULL);



    /* Pre-compute the DFT coefficients. We will only use some of them in
     * sliding_dft().
     */
    for (i = 0; i < dft_points; i++)
        coeffs[i] = cexp(I * 2. * M_PI * i / dft_points);

    /* Read chunks of data piped from rtl_sdr utility and call sliding_dft()
     * for each sample. The data comes in I/Q pairs, like: IQIQIQIQIQ...
     * Individual values (either I or Q) range is (0, 255), and to convert to
     * signed we need to subtract 127. No idea why RTL-SDR dongle doesn't use
     * signed integer by default (looks like the hardware itself returns the
     * data in this way).
     */



    //char *cmd = "rtl_sdr -f 868.05m -s 1.6m -g 14 -p 0 -";

    char cmd[1000];
    
    sprintf(cmd, "rtl_sdr -f 868.05m -s 1.6m");
    
    if(DumpFLARM.gain!=0){

        sprintf(cmd, "%s -g %d",cmd,DumpFLARM.gain);

        printf("%s\n",cmd);
        
    }

    if(DumpFLARM.dev_name!=0){

        sprintf(cmd, "%s -d %s",cmd,DumpFLARM.dev_name);

    }

    if(DumpFLARM.ppm_error!=0){

        sprintf(cmd, "%s -p %d",cmd,DumpFLARM.ppm_error);

    }

    if(DumpFLARM.enable_rtlsdr_biast) {

        sprintf(cmd, "%s -B 1",cmd);

    }

    if(DumpFLARM.other_options!=0) {

        sprintf(cmd, "%s %s",cmd,DumpFLARM.other_options);

    }


    sprintf(cmd, "%s -",cmd);

    FILE *fp;

    if ((fp = popen(cmd, "r")) == NULL) {
        printf("Error opening pipe!\n");
        return -1;
    }


    while (true) {


        len = fread(raw_buffer, sizeof(raw_buffer[0]), sizeof(raw_buffer), fp);
        for (i = 0; i < len; i += 2)
            sliding_dft(raw_buffer[i] - 127, raw_buffer[i + 1] - 127);

    }



    return 0;
}

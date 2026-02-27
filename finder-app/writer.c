#include <syslog.h>
#include <stdio.h>

int main(int argc, char* argv[])
{
    openlog("Assignment2", LOG_PID | LOG_CONS, LOG_USER);

    if(argc < 2) {
        syslog(LOG_ERR, "No string argument!");

        if(argc < 1) {
            syslog(LOG_ERR, "No file name argument!");
        }
        
        return 1;
    }

    char* file_name = argv[1];
    char* write_string = argv[2];

    printf("File name: %s\n", file_name);
    FILE *fptr = fopen(file_name, "w");

    if(fptr == NULL) {
        syslog(LOG_ERR, "File open error!");
        return 1;
    }
    else {
        fprintf(fptr, "%s\n", write_string);
    }

    fclose(fptr);
    closelog();

}

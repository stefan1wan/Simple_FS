#include <stdio.h>

static int do_decode(unsigned char* buf, int length){
	unsigned int mask = 0xdeedbeef;
	for(int i=0; i<length; i++){
		unsigned char* buf = &buffer[i];
		buf[i]  = (buf[i] >> 1) | (buf[i] << 7);
		buf[i] ^= (unsigned char)(mask&0xff);
		buf[i]  = (buf[i] >> 2) | (buf[i] << 6);
		buf[i] ^= (unsigned char)((mask>>8)&0xff);
		buf[i]  = (buf[i] >> 3) | (buf[i] << 5);
		buf[i] ^= (unsigned char)((mask>>16)&0xff);
		buf[i]  = (buf[i] >> 4) | (buf[i] << 4);
		buf[i] ^= (unsigned char)((mask>>24)&0xff);
		buf[i]  = (buf[i] >> 5) | (buf[i] << 3);
	}
	return 0;
}

int main(){
    int fd = open("output_file", r);
    unsigned char* buf[10000];
    int len = read(fd, buf,100);
    do_decode(buf, len);
    printf("%s\n", buf);
}
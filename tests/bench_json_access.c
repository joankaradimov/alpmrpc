/* aj_elem(d, arr, i) walks the sibling chain from the start every time, so a
 * loop over it is O(n^2). Does that matter at the sizes we actually send? */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "arpc_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double f_;
static double us(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return t.QuadPart/f_;}
static char *mk(int n, int strings, size_t *len){
	aj_w w; ajw_init(&w); ajw_arr_begin(&w);
	for(int i=0;i<n;i++){
		if(strings){char b[64];snprintf(b,sizeof(b),"mingw-w64-ucrt-x86_64-package-%d",i);ajw_str(&w,b);}
		else ajw_i64(&w,100000+i);
	}
	ajw_arr_end(&w); *len=w.len; return w.buf;
}
int main(void){
	LARGE_INTEGER f;QueryPerformanceFrequency(&f);f_=f.QuadPart/1e6;
	printf("  %-10s %-9s %10s %10s %10s\n","elements","payload","parse","aj_elem","walk");
	int sizes[]={100,500,1214,5000};
	for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);s++){
		int n=sizes[s]; size_t len; char *j=mk(n,1,&len);
		const int R=200; double t0,t1;
		t0=us(); for(int r=0;r<R;r++){aj_doc d;aj_parse(&d,j,len);aj_free(&d);} t1=us();
		double parse=(t1-t0)/R;
		aj_doc d; aj_parse(&d,j,len);
		t0=us();
		for(int r=0;r<R;r++){volatile size_t acc=0;
			for(int i=0;i<n;i++){const char*v=aj_str(&d,aj_elem(&d,0,i),NULL);acc+=(size_t)v;} }
		t1=us(); double byidx=(t1-t0)/R;
		t0=us();
		for(int r=0;r<R;r++){volatile size_t acc=0;
			for(int i=d.nodes[0].first_child;i>=0;i=d.nodes[i].next_sibling)
				acc+=(size_t)aj_str(&d,i,NULL); }
		t1=us(); double bywalk=(t1-t0)/R;
		aj_free(&d);
		printf("  %-10d %7.1f KB %8.1f us %8.1f us %8.1f us\n",
		       n,len/1024.0,parse,byidx,bywalk);
		free(j);
	}
	return 0;
}

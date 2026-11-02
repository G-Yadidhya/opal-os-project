#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT "results/raw/opal_bptree_hotkey_comparison.csv"
#define RUNS 5
#define DISTS 2
#define MODES 2
#define TOTAL_OPS 40000
#define HEADER "pair,order,distribution,mode,run,threads,operations_per_thread,lookup_pct,update_pct,insert_pct,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec"
typedef struct { double elapsed, throughput; unsigned long retries, batches; } Measurement;
static const char *dist_name[DISTS] = {"uniform", "skew80_20"};
static int cmp(const void *a, const void *b) { double x=*(const double *)a, y=*(const double *)b; return (x>y)-(x<y); }
static double median(double v[RUNS]) { qsort(v,RUNS,sizeof(*v),cmp); return v[RUNS/2]; }
int main(void) {
  FILE *f=fopen(INPUT,"r"); if(!f){perror(INPUT);return EXIT_FAILURE;}
  char line[768]; if(!fgets(line,sizeof line,f)){fprintf(stderr,"ERROR: Empty CSV.\n");fclose(f);return EXIT_FAILURE;}
  line[strcspn(line,"\r\n")]=0; if(strcmp(line,HEADER)){fprintf(stderr,"ERROR: Unexpected CSV header.\n");fclose(f);return EXIT_FAILURE;}
  Measurement d[DISTS][MODES][RUNS]={{{{0}}}}; int seen[DISTS][MODES][RUNS]={{{0}}}, rows=0;
  while(fgets(line,sizeof line,f)) {
    line[strcspn(line,"\r\n")]=0; int pair,run,threads,ops,lp,up,ip,lo,uo,io,used=0;
    char order[32]={0},dist[24]={0},mode[16]={0}; unsigned long retries,callbacks,batches; double elapsed,throughput;
    int n=sscanf(line,"%d,%31[^,],%23[^,],%15[^,],%d,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lf,%lf%n",
      &pair,order,dist,mode,&run,&threads,&ops,&lp,&up,&ip,&lo,&uo,&io,&retries,&callbacks,&batches,&elapsed,&throughput,&used);
    if(n!=18||line[used]) { fprintf(stderr,"ERROR: Malformed CSV row %d.\n",rows+1);fclose(f);return EXIT_FAILURE; }
    int di=!strcmp(dist,dist_name[0])?0:!strcmp(dist,dist_name[1])?1:-1;
    int mi=!strcmp(mode,"mutex")?0:!strcmp(mode,"opal")?1:-1;
    const char *want_order=pair%2?"mutex_then_opal":"opal_then_mutex";
    if(di<0||mi<0||pair<1||pair>RUNS||run!=pair||threads!=4||ops!=10000||lp!=50||up!=40||ip!=10||lo!=20000||uo!=16000||io!=4000||callbacks!=16000||strcmp(order,want_order)||elapsed<=0||throughput<=0||fabs(throughput-TOTAL_OPS/elapsed)>0.02||(mi==0&&(retries||batches))||(mi==1&&!batches)||seen[di][mi][pair-1]) {
      fprintf(stderr,"ERROR: Invalid CSV row %d.\n",rows+1);fclose(f);return EXIT_FAILURE;
    }
    d[di][mi][pair-1]=(Measurement){elapsed,throughput,retries,batches}; seen[di][mi][pair-1]=1; rows++;
  }
  if(ferror(f)){perror("CSV read");fclose(f);return EXIT_FAILURE;} fclose(f);
  if(rows!=DISTS*MODES*RUNS){fprintf(stderr,"ERROR: Expected 20 measurements, found %d.\n",rows);return EXIT_FAILURE;}
  for(int a=0;a<DISTS;a++)for(int b=0;b<MODES;b++)for(int c=0;c<RUNS;c++)if(!seen[a][b][c]){fprintf(stderr,"ERROR: Missing record.\n");return EXIT_FAILURE;}
  printf("OPAL-Inspired B+ Tree Hot-Key Comparison\nValidated measurements: %d\n",rows);
  for(int a=0;a<DISTS;a++) {
    double times[MODES][RUNS], rates[MODES][RUNS], retries=0,batches=0;
    for(int b=0;b<MODES;b++)for(int c=0;c<RUNS;c++){times[b][c]=d[a][b][c].elapsed;rates[b][c]=d[a][b][c].throughput;if(b){retries+=d[a][b][c].retries;batches+=d[a][b][c].batches;}}
    double mt=median(times[0]),ot=median(times[1]),mr=median(rates[0]),orr=median(rates[1]);
    printf("\nDistribution: %s\nStrategy      Median time (s)  Median throughput (ops/s)\n",dist_name[a]);
    printf("mutex         %.9f       %.2f\nOPAL-inspired %.9f       %.2f\n",mt,mr,ot,orr);
    printf("Throughput ratio (OPAL/mutex): %.2fx\nElapsed-time change (OPAL vs mutex): %.2f%%\n",orr/mr,100.0*(mt-ot)/mt);
    printf("OPAL average retries/run: %.0f; average batches/run: %.0f\n",retries/RUNS,batches/RUNS);
  }
  puts("\nScope: four threads, balanced 50/40/10 mix, 40,000 operations/run; local C11 teaching adaptation, not upstream OPAL.\nANALYSIS: PASS");
  return EXIT_SUCCESS;
}

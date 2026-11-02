#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT "results/raw/opal_bptree_batch_policy_comparison_per_batch.csv"
#define RUNS 5
#define HEADER "pair,order,policy,run,update_callbacks,update_batches,singleton_batches,max_batch_size,bucket_1,bucket_2,bucket_3_4,bucket_5_8,bucket_9_plus,update_latency_median_ns,update_latency_p95_ns,elapsed_seconds,throughput_ops_sec"
typedef struct { unsigned long batches,singles,maximum,buckets[5],latency50,latency95; double elapsed,throughput; } Row;
static int cmp_double(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return(x>y)-(x<y);}
static int cmp_ulong(const void*a,const void*b){unsigned long x=*(const unsigned long*)a,y=*(const unsigned long*)b;return(x>y)-(x<y);}
static double med_double(double a[RUNS]){qsort(a,RUNS,sizeof(*a),cmp_double);return a[RUNS/2];}
static unsigned long med_ulong(unsigned long a[RUNS]){qsort(a,RUNS,sizeof(*a),cmp_ulong);return a[RUNS/2];}

int main(void){
  FILE*f=fopen(INPUT,"r");if(!f){perror(INPUT);return EXIT_FAILURE;}
  char line[900];if(!fgets(line,sizeof line,f)){fprintf(stderr,"ERROR: Empty CSV.\n");fclose(f);return EXIT_FAILURE;}
  line[strcspn(line,"\r\n")]=0;if(strcmp(line,HEADER)){fprintf(stderr,"ERROR: CSV header mismatch.\n");fclose(f);return EXIT_FAILURE;}
  Row data[2][RUNS]={{{0}}};int seen[2][RUNS]={{0}},rows=0;
  while(fgets(line,sizeof line,f)){
    line[strcspn(line,"\r\n")]=0;int pair,run,callbacks,used=0;char order[48]={0},policy[24]={0};
    unsigned long batches,singles,maximum,b1,b2,b34,b58,b9,lat50,lat95;double elapsed,throughput;
    int n=sscanf(line,"%d,%47[^,],%23[^,],%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lf,%lf%n",
      &pair,order,policy,&run,&callbacks,&batches,&singles,&maximum,&b1,&b2,&b34,&b58,&b9,&lat50,&lat95,&elapsed,&throughput,&used);
    if(n!=17||line[used]){fprintf(stderr,"ERROR: Malformed CSV row %d.\n",rows+1);fclose(f);return EXIT_FAILURE;}
    int p=!strcmp(policy,"immediate")?0:!strcmp(policy,"collect1us")?1:-1;
    const char*want_order=pair%2?"immediate_then_collect1us":"collect1us_then_immediate";
    unsigned long bucket_sum=b1+b2+b34+b58+b9;
    if(p<0||pair<1||pair>RUNS||run!=pair||strcmp(order,want_order)||callbacks!=16000||
       lat50==0||lat95<lat50||elapsed<=0||throughput<=0||fabs(throughput-40000.0/elapsed)>0.02||seen[p][pair-1]||
       batches==0||singles!=b1||bucket_sum!=batches||maximum==0||
       (b1&&maximum<1)||(b2&&maximum<2)||(b34&&maximum<3)||(b58&&maximum<5)||(b9&&maximum<9)){
      fprintf(stderr,"ERROR: Invalid CSV row %d.\n",rows+1);fclose(f);return EXIT_FAILURE;
    }
    data[p][pair-1]=(Row){batches,singles,maximum,{b1,b2,b34,b58,b9},lat50,lat95,elapsed,throughput};seen[p][pair-1]=1;rows++;
  }
  if(ferror(f)){perror("CSV read");fclose(f);return EXIT_FAILURE;}fclose(f);
  if(rows!=10){fprintf(stderr,"ERROR: Expected 10 records; found %d.\n",rows);return EXIT_FAILURE;}
  for(int p=0;p<2;p++)for(int r=0;r<RUNS;r++)if(!seen[p][r]){fprintf(stderr,"ERROR: Missing record.\n");return EXIT_FAILURE;}
  double elapsed[2][RUNS],throughput[2][RUNS];unsigned long p50[2][RUNS],p95[2][RUNS],batch[2][RUNS],single[2][RUNS],maxbatch[2][RUNS];
  for(int p=0;p<2;p++)for(int r=0;r<RUNS;r++){Row x=data[p][r];elapsed[p][r]=x.elapsed;throughput[p][r]=x.throughput;p50[p][r]=x.latency50;p95[p][r]=x.latency95;batch[p][r]=x.batches;single[p][r]=x.singles;maxbatch[p][r]=x.maximum;}
  double ti=med_double(elapsed[0]),tc=med_double(elapsed[1]),ri=med_double(throughput[0]),rc=med_double(throughput[1]);
  unsigned long i50=med_ulong(p50[0]),c50=med_ulong(p50[1]),i95=med_ulong(p95[0]),c95=med_ulong(p95[1]);
  printf("OPAL-Inspired Batch Collection Policy Comparison\nValidated measurements: %d\n",rows);
  printf("Policy       Median throughput   Median elapsed   Update latency p50   Update latency p95   Median batches   Median singleton batches   Median max batch\n");
  printf("immediate    %.2f ops/s    %.9f s    %lu ns    %lu ns    %lu    %lu    %lu\n",ri,ti,i50,i95,med_ulong(batch[0]),med_ulong(single[0]),med_ulong(maxbatch[0]));
  printf("collect1us   %.2f ops/s    %.9f s    %lu ns    %lu ns    %lu    %lu    %lu\n",rc,tc,c50,c95,med_ulong(batch[1]),med_ulong(single[1]),med_ulong(maxbatch[1]));
  printf("Throughput ratio collect1us/immediate: %.3fx\n",rc/ri);
  printf("Update p50 latency change: %.2f%%\nUpdate p95 latency change: %.2f%%\n",100.0*((double)c50/(double)i50-1.0),100.0*((double)c95/(double)i95-1.0));
  puts("Scope: one machine, balanced 50/40/10 workload, four threads; experimental 1us spin-collection clone, not upstream OPAL. ANALYSIS: PASS");
  return EXIT_SUCCESS;
}

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT "results/raw/opal_bptree_batch_metrics_low_overhead.csv"
#define RUNS 5
#define HEADER "pair,order,mode,run,update_callbacks,update_batches,singleton_batches,max_batch_size,bucket_1,bucket_2,bucket_3_4,bucket_5_8,bucket_9_plus,elapsed_seconds,throughput_ops_sec"
typedef struct { unsigned long batches, singles, maximum, buckets[5]; double elapsed, throughput; } Row;
static int cmp_double(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static int cmp_ulong(const void *a,const void *b){unsigned long x=*(const unsigned long *)a,y=*(const unsigned long *)b;return (x>y)-(x<y);}
static double median_double(double a[RUNS]){qsort(a,RUNS,sizeof(*a),cmp_double);return a[RUNS/2];}
static unsigned long median_ulong(unsigned long a[RUNS]){qsort(a,RUNS,sizeof(*a),cmp_ulong);return a[RUNS/2];}

int main(void){
  FILE *f=fopen(INPUT,"r");if(!f){perror(INPUT);return EXIT_FAILURE;}
  char line[768];if(!fgets(line,sizeof line,f)){fprintf(stderr,"ERROR: Empty CSV.\n");fclose(f);return EXIT_FAILURE;}
  line[strcspn(line,"\r\n")]=0;if(strcmp(line,HEADER)){fprintf(stderr,"ERROR: CSV header mismatch.\n");fclose(f);return EXIT_FAILURE;}
  Row rows[2][RUNS]={{{0}}};int seen[2][RUNS]={{0}},count=0;
  while(fgets(line,sizeof line,f)){
    line[strcspn(line,"\r\n")]=0;int pair,run,callbacks,used=0;char order[32]={0},mode[16]={0};
    unsigned long batches,singles,maximum,b1,b2,b34,b58,b9;double elapsed,throughput;
    int n=sscanf(line,"%d,%31[^,],%15[^,],%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lf,%lf%n",
      &pair,order,mode,&run,&callbacks,&batches,&singles,&maximum,&b1,&b2,&b34,&b58,&b9,&elapsed,&throughput,&used);
    if(n!=15||line[used]){fprintf(stderr,"ERROR: Malformed CSV row %d.\n",count+1);fclose(f);return EXIT_FAILURE;}
    int m=!strcmp(mode,"mutex")?0:!strcmp(mode,"opal")?1:-1;
    const char *expected_order=pair%2?"mutex_then_opal":"opal_then_mutex";
    unsigned long bucket_sum=b1+b2+b34+b58+b9;
    if(m<0||pair<1||pair>RUNS||run!=pair||strcmp(order,expected_order)||callbacks!=16000||elapsed<=0||throughput<=0||fabs(throughput-40000.0/elapsed)>0.02||seen[m][pair-1]||
       (m==0&&(batches||singles||maximum||bucket_sum))||
       (m==1&&(!batches||singles!=b1||bucket_sum!=batches||!maximum||(b1>0&&maximum<1)||(b2>0&&maximum<2)||(b34>0&&maximum<3)||(b58>0&&maximum<5)||(b9>0&&maximum<9)))){
      fprintf(stderr,"ERROR: Invalid CSV row %d.\n",count+1);fclose(f);return EXIT_FAILURE;
    }
    rows[m][pair-1]=(Row){batches,singles,maximum,{b1,b2,b34,b58,b9},elapsed,throughput};seen[m][pair-1]=1;count++;
  }
  if(ferror(f)){perror("CSV read");fclose(f);return EXIT_FAILURE;}fclose(f);
  if(count!=2*RUNS){fprintf(stderr,"ERROR: Expected 10 rows; found %d.\n",count);return EXIT_FAILURE;}
  for(int m=0;m<2;m++)for(int r=0;r<RUNS;r++)if(!seen[m][r]){fprintf(stderr,"ERROR: Missing row.\n");return EXIT_FAILURE;}

  double elapsed[RUNS],throughput[RUNS];unsigned long batches[RUNS],singles[RUNS],maximum[RUNS],bucket_total[5]={0};
  for(int r=0;r<RUNS;r++){Row x=rows[1][r];elapsed[r]=x.elapsed;throughput[r]=x.throughput;batches[r]=x.batches;singles[r]=x.singles;maximum[r]=x.maximum;for(int b=0;b<5;b++)bucket_total[b]+=x.buckets[b];}
  double med_time=median_double(elapsed),med_rate=median_double(throughput);
  unsigned long med_batches=median_ulong(batches),med_singles=median_ulong(singles),med_max=median_ulong(maximum);
  unsigned long total_batches=0;for(int b=0;b<5;b++)total_batches+=bucket_total[b];
  printf("OPAL-Inspired C11 B+ Tree Update Batch Metrics\nValidated measurements: %d\n",count);
  printf("OPAL median batches/run: %lu\nOPAL median singleton batches/run: %lu\nOPAL median maximum batch size: %lu\n",med_batches,med_singles,med_max);
  printf("Singleton share across measured batches: %.2f%%\n",100.0*(double)bucket_total[0]/(double)total_batches);
  printf("Batch-size histogram (mean batches/run): 1=%.1f, 2=%.1f, 3-4=%.1f, 5-8=%.1f, 9+=%.1f\n",bucket_total[0]/5.0,bucket_total[1]/5.0,bucket_total[2]/5.0,bucket_total[3]/5.0,bucket_total[4]/5.0);
  printf("OPAL median elapsed: %.9f s; median throughput: %.2f ops/s\n",med_time,med_rate);
  puts("Scope: existing immediate-drain policy; counters add no waiting or batching delay. ANALYSIS: PASS");
  return EXIT_SUCCESS;
}

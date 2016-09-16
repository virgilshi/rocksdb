@files = ("insert_emon", "overwrite_emon", "readwrite_emon");
#$results_dir = "/home/ptaylor/kvs_sept/rocksdb/results/testrun_20160915_152837";

if (@ARGV != 1) {
   print "error: parse_emon_rocksdb.pl requires command line argument.\n";
   print "usage: perl parse_emon_rocksdb.pl <PATH TO RESULT FILES>\n"; 
   exit;
}

$results_dir = $ARGV[0];


foreach $file(@files) {
   $n = "$results_dir"."/"."$file".".txt";

   $o = "$results_dir"."/"."$file"."_parsed".".tsv";
   print "Parsing $n\n";
   $print_event = 1;
   $sample = 0;

   open (INFILE, $n) or die $!;
   open (OUTFILE, '>' , $o) or die $!;


   print OUTFILE "$sample\t";
   $sample++;
   
   foreach $line (<INFILE>)  
   {  
      
      chomp($line);
      @linedata = split('\t',$line);
      #process only lines with emon data
      if ($linedata[1] !~/^$/) {
         
   
         $event = shift @linedata;
         $clocks = shift @linedata;
   
         #split array in half to get counts from each socket
         @S0 = @linedata;
         @S1 = splice @S0, scalar(@linedata)/2;
   
         #sum emon data 
         $S0_sum=0;
         $S1_sum=0;
         foreach (@S0){
            $S0_sum += $_;
         }
         foreach (@S1){
            $S1_sum += $_;
         }
   
         #change to MB/s
         #for QPI, multiply each count by 8 bytes. NOT valid for SKX/Purley
         if ($event =~ /UNC_Q/) {
            $S0_sum = int(($S0_sum*8/1000000)+0.5);
            $S1_sum = int(($S1_sum*8/1000000)+0.5);
         }
         #multiply each count by 64 bytes.
         else {
            $S0_sum = int(($S0_sum*64/1000000)+0.5);
            $S1_sum = int(($S1_sum*64/1000000)+0.5);
         }

         #only print events where MB/s is relevent 
         if ($event =~ /LONGEST_LAT_CACHE/ || $event =~ /CAS_COUNT/ || $event =~ /H_REQUESTS/ || $event =~ /UNC_Q/) {
         
            #print event name header - note that we do not print the data with the first sample 
            if ($print_event) {
               #using two tabs to cover 2 socket data under this event header.
               print OUTFILE "$event\t\t";
            }
            else {
               print OUTFILE "$S0_sum\t$S1_sum\t";
            }
         }
         
      }
      #start new line each sample interval
      if ($linedata[0] eq "==========") {
         print OUTFILE "\n";
         $print_event=0;
         print OUTFILE "$sample\t";
         $sample++;
      }
      
   }
   close OUTFILE;
   close INFILE;
}



#plot 'out.dat' using 1:1 with linespoints title "realtime", \
#     'out.dat' using 1:2 with linespoints title "audio time", \
#     'out.dat' using 1:3 with linespoints title "buffer time"

plot 1 with linespoints title "realtime", \
     'out.dat' using 1:($2 - $1) with linespoints title "audio time", \
     'out.dat' using 1:($3 - $1) with linespoints title "buffer time"

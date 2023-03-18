# LineCounter
CMD utility to determine the total source lines of code (sloc) of C/C++ projects.
The application reads from a line_count.in file the root paths of the projects that should be searched. It goes recursively in every .h, .c, .cpp or .hpp file contained in those paths and determines their sloc for each file. At the end it will print the total line count and the amount of time needed to perform the task. It uses multithreading to speed up the IO operations and the sloc determination. It will print the output into a line_count.out file as well such that you can inspect the values at your leisure. Look at line_count.out for an example output.

Example Output
(line_count.out)
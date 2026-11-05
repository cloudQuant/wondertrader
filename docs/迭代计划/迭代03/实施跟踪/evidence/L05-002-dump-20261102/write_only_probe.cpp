#include "Share/BoostFile.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
int main() {
 const char* path = "/tmp/wt-i03-l05-dump-20261102/write-only.csv";
 { std::ofstream out(path); out << "seed"; }
 if (geteuid()==0 || chmod(path, S_IWUSR) != 0) return 2;
 BoostFile previous; const bool boost_open = previous.create_new_file(path);
 previous.close_file();
 std::ofstream current(path, std::ios::binary); const bool output_open = current.is_open();
 current << "updated"; current.close(); const bool output_ok = !current.fail();
 chmod(path, S_IRUSR|S_IWUSR); unlink(path);
 std::cout << "BoostFile read_write: " << boost_open << "\nofstream write_only: " << output_open << "\nofstream write/close: " << output_ok << '\n';
 return !(!boost_open && output_open && output_ok);
}

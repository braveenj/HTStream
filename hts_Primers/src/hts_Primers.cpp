#include <iostream>
#include <string>
#include <boost/program_options.hpp>
#include <vector>
#include <fstream>
#include "ioHandler.h"
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <map>
#include <unordered_map>
#include <boost/functional/hash.hpp>

#include "hts_Primers.h"

namespace
{
    const size_t SUCCESS = 0;
    const size_t ERROR_IN_COMMAND_LINE = 1;
    const size_t ERROR_UNHANDLED_EXCEPTION = 2;

} // namespace

namespace bf = boost::filesystem;
namespace bi = boost::iostreams;

int main(int argc, char** argv)
{

    const std::string program_name = "hts_Primers";
    std::string app_description =
                       "The hts_Primers application identifies primer sequences located on the 5' ends of R1 and R2,\n";
    app_description += "    or 5' and 3' end of SE reads, optionally cut/flip and return the the read adding the \n";
    app_description += "    primer to the read id.\n";

    try
    {
        /** Define and parse the program options
         */
        namespace po = boost::program_options;
        po::options_description standard = setStandardOptions();
            // version|v ; help|h ; notes|N ; stats-file|L ; append-stats-file|A
        po::options_description input = setInputOptions();
            // read1-input|1 ; read2-input|2 ; singleend-input|U
            // tab-input|T ; interleaved-input|I
        po::options_description output = setOutputOptions(program_name);
            // force|F ; prefix|p ; gzip-output,g ; fastq-output|f
            // tab-output|t ; interleaved-output|i ; unmapped-output|u ; to-stdout,O

        po::options_description desc("Application Specific Options");

        setDefaultParamsCutting(desc);
            // no-orphans|n ; stranded|s ; min-length|m

        desc.add_options()
            ("primers_5p,P", po::value<std::string>(), "5' primers, comma separated list of sequences, or fasta file");
        desc.add_options()
            ("primers_3p,Q", po::value<std::string>(), "3' primers, comma separated list of sequences, or fasta file");
        desc.add_options()
            ("primer_mismatches,d", po::value<size_t>()->default_value(4)->notifier(boost::bind(&check_range<size_t>, "primer_mismatches", _1, 0, 10000)), "Max hamming dist from primer (min 0, max 10000)");
        desc.add_options()
            ("primer_end_mismatches,e", po::value<size_t>()->default_value(4)->notifier(boost::bind(&check_range<size_t>, "primer_end_mismatches", _1, 0, 10000)), "Required number of matching bases at end of primer (min 0, max 10000)");
        desc.add_options()
            ("float,l", po::value<size_t>()->default_value(0)->notifier(boost::bind(&check_range<size_t>, "float", _1, 0, 10000)), "Variable number of bases preceeding primer allowed to float");
        desc.add_options()
            ("flip,x", po::bool_switch()->default_value(false), "Primers can be seen in both orientiations, tests flip and reorients all reads to the same orientation.");
        desc.add_options()
            ("keep,k", po::bool_switch()->default_value(false), "Don't cut off the primer sequence, leave it as a part of the read");
        desc.add_options()
            ("min_primer_matches,r", po::value<size_t>()->default_value(0)->notifier(boost::bind(&check_range<size_t>, "min_primer_matches", _1, 0, 2)), "Minimum number of primers to match to keep the fragment (0, keep all fragments, 1 must match either 5' or 3' primer, 2 must match both 5' and 3' primers)");

        po::options_description cmdline_options;
        cmdline_options.add(standard).add(input).add(output).add(desc);

        po::variables_map vm;
        try
        {
            po::store(po::parse_command_line(argc, argv, cmdline_options), vm); // can throw

            /** --help option
             */
            version_or_help(program_name, app_description, cmdline_options, vm);
            po::notify(vm); // throws on error, so do after help in case

            std::shared_ptr<OutputWriter> pe = nullptr;
            std::shared_ptr<OutputWriter> se = nullptr;
            outputWriters(pe, se, vm);

            std::string statsFile(vm["stats-file"].as<std::string>());
            PrimerCounters counters(statsFile, vm["force"].as<bool>(), vm["append-stats-file"].as<bool>(), program_name, vm["notes"].as<std::string>());

            SeqMap p5_primer_sequence;
            if (vm.count("primers_5p")) {
                p5_primer_sequence = fasta2dict(vm["primers_5p"].as<std::string>());
            }

            SeqMap p3_primer_sequence;
            if (vm.count("primers_3p")) {
                p3_primer_sequence = fasta2dict(vm["primers_3p"].as<std::string>());
            }

            counters.set_keep_primer(vm["keep"].as<bool>());
            counters.set_seqmap(p5_primer_sequence,p3_primer_sequence);

            if(vm.count("read1-input")) {
                if (!vm.count("read2-input")) {
                    throw std::runtime_error("must specify both read1 and read2 input files.");
                }
                auto read1_files = vm["read1-input"].as<std::vector<std::string> >();
                auto read2_files = vm["read2-input"].as<std::vector<std::string> >();
                if (read1_files.size() != read2_files.size()) {
                    throw std::runtime_error("must have same number of input files for read1 and read2");
                }
                for(size_t i = 0; i < read1_files.size(); ++i) {
                    bi::stream<bi::file_descriptor_source> is1{check_open_r(read1_files[i]), bi::close_handle};
                    bi::stream<bi::file_descriptor_source> is2{check_open_r(read2_files[i]), bi::close_handle};
                    InputReader<PairedEndRead, PairedEndReadFastqImpl> ifp(is1, is2);
                    helper_Primers(ifp, pe, se, counters, vm, p5_primer_sequence, p3_primer_sequence);
                }
            }
            if (vm.count("interleaved-input")) {
                auto read_files = vm["interleaved-input"].as<std::vector<std::string > >();
                for (auto file : read_files) {
                    bi::stream<bi::file_descriptor_source> inter{ check_open_r(file), bi::close_handle};
                    InputReader<PairedEndRead, InterReadImpl> ifi(inter);
                    helper_Primers(ifi, pe, se, counters, vm, p5_primer_sequence, p3_primer_sequence);
                }
            }
            if(vm.count("singleend-input")) {
                auto read_files = vm["singleend-input"].as<std::vector<std::string> >();
                for (auto file : read_files) {
                    bi::stream<bi::file_descriptor_source> sef{ check_open_r(file), bi::close_handle};
                    InputReader<SingleEndRead, SingleEndReadFastqImpl> ifs(sef);
                    helper_Primers(ifs, pe, se, counters, vm, p5_primer_sequence, p3_primer_sequence);
                }
            }
            if(vm.count("tab-input")) {
                auto read_files = vm["tab-input"].as<std::vector<std::string> > ();
                for (auto file : read_files) {
                    bi::stream<bi::file_descriptor_source> tabin{ check_open_r(file), bi::close_handle};
                    InputReader<ReadBase, TabReadImpl> ift(tabin);
                    helper_Primers(ift, pe, se, counters, vm, p5_primer_sequence, p3_primer_sequence);
                }
            }
            if (!isatty(fileno(stdin))) {
                bi::stream<bi::file_descriptor_source> tabin {fileno(stdin), bi::close_handle};
                InputReader<ReadBase, TabReadImpl> ift(tabin);
                helper_Primers(ift, pe, se, counters, vm, p5_primer_sequence, p3_primer_sequence);
            }
            counters.write_out();
        }
        catch(po::error& e)
        {
            std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
            return ERROR_IN_COMMAND_LINE;
        }
    }
    catch(std::exception& e)
    {
        std::cerr << "\n\tUnhandled Exception: "
                  << e.what() << std::endl;
        return ERROR_UNHANDLED_EXCEPTION;

    }

    return SUCCESS;

}

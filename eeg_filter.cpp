#include <iostream>
#include <fstream>
#include <string>
#include <stdio.h>
#include <boost/circular_buffer.hpp>
#include <boost/numeric/ublas/matrix.hpp>
#include <boost/numeric/ublas/io.hpp>
#include <boost/lexical_cast.hpp>
#include <Iir.h>
#include <Fir1.h>
#include <chrono>
#include <string>
#include <ctime>
#include <memory>
#include <numeric>
#include "Neuron.h"
#include "Layer.h"
#include "Net.h"
#include "parameters.h"
#include "dynamicPlots.h"

#define CVUI_IMPLEMENTATION
#include "cvui.h"

using namespace std;
constexpr int ESC_key = 27;

// PLOT
#ifdef DoShowPlots
#define WINDOW "Deep Neuronal Filter"
const int plotW = 1200/2;
const int plotH = 720;
#endif


// take from http://stackoverflow.com/a/236803/248823
void split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss;
    ss.str(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
}



void processOneSubject(int subjIndex, const char* filename) {
	std::srand(1);

	// file path prefix for the results
	std::string outpPrefix = "results";

	int fs = 250;
	if (NULL != filename) {
		fs = 500;
		outpPrefix = filename;
	}

	const int samplesNoLearning = 3 * fs / innerHighpassCutOff;
	
	const int outerDelayLineLength = fs / outerHighpassCutOff;
	fprintf(stderr,"outerDelayLineLength = %d\n",outerDelayLineLength);
	
	const int innerDelayLineLength = outerDelayLineLength / 2;

	boost::circular_buffer<double> oo_buf(bufferLength);
	boost::circular_buffer<double> io_buf(bufferLength);
	boost::circular_buffer<double> ro_buf(bufferLength);
	boost::circular_buffer<double> f_nno_buf(bufferLength);
//LMS
	boost::circular_buffer<double> lms_o_buf(bufferLength);
	
//adding delay line for the noise
	double outer_delayLine[outerDelayLineLength] = {0.0};
	boost::circular_buffer<double> innertrigger_delayLine(innerDelayLineLength);
	boost::circular_buffer<double> inner_delayLine(innerDelayLineLength);
	
// FILES
	fstream nn_file;
	fstream remover_file;
	fstream inner_file;
	fstream outer_file;
	fstream lms_file;
	fstream lms_remover_file;
	fstream laplace_file;
	ifstream p300_infile;
	fstream wdistance_file;
#ifdef SAVE_WEIGHTS
	fstream weight_file;
#endif

long count = 0;
	//setting up the interactive window and the dynamic plot class
#ifdef DoShowPlots
	auto frame = cv::Mat(cv::Size(plotW, plotH), CV_8UC3);
	cvui::init(WINDOW, 1);
	dynaPlots plots(frame, plotW, plotH);
#endif
	//create files for saving the data and parameters
	string sbjct = std::to_string(subjIndex);

        //NN specifications
	int nNeurons[NLAYERS];
	// calc an exp reduction of the numbers always reaching 1
	double b = exp(log(outerDelayLineLength)/(NLAYERS-1));
	for(int i=0;i<NLAYERS;i++) {
		nNeurons[i] = outerDelayLineLength / pow(b,i);
		if (i == (NLAYERS-1)) nNeurons[i] = 1;
		fprintf(stderr,"Layer %d has %d neurons.\n",i,nNeurons[i]);
	}

	//create the neural network
	Net NNO(NLAYERS, nNeurons, outerDelayLineLength, 0, "P300");
	
	//setting up the neural networks
	NNO.initNetwork(Neuron::W_RANDOM, Neuron::B_NONE, Neuron::Act_ReLU);
		
	nn_file.open(outpPrefix+"/subject" + sbjct + "/fnn.tsv", fstream::out);
	remover_file.open(outpPrefix+"/subject" + sbjct + "/remover.tsv", fstream::out);
	inner_file.open(outpPrefix+"/subject" + sbjct + "/inner.tsv", fstream::out);
	outer_file.open(outpPrefix+"/subject" + sbjct + "/outer.tsv", fstream::out);
	lms_file.open(outpPrefix+"/subject" + sbjct + "/lmsOutput.tsv", fstream::out);
	lms_remover_file.open(outpPrefix+"/subject" + sbjct + "/lmsCorrelation.tsv", fstream::out);
	laplace_file.open(outpPrefix+"/subject" + sbjct + "/laplace.tsv", fstream::out);
#ifdef SAVE_WEIGHTS
	weight_file.open(outpPrefix+"/subject" + sbjct + "/lWeights.tsv", fstream::out);
#endif
	wdistance_file.open(outpPrefix+"/subject" + sbjct + "/weight_distance.tsv", fstream::out);
	
	char tmp[256];
	if (NULL != filename) {
		sprintf(tmp,"../noisewalls/EEG_recordings/participant%03d/%s.tsv",subjIndex,filename);
	} else {
		sprintf(tmp,"../noisewalls/EEG_recordings/participant%03d/rawp300.tsv",subjIndex);
	}
	p300_infile.open(tmp);
	if (!p300_infile) {
		cout << "Unable to open file: " << tmp << endl;
		exit(1); // terminate with error
	}
	
	//setting up all the filters required
	Iir::Butterworth::HighPass<filterorder> outer_filterHP;
	outer_filterHP.setup(fs,outerHighpassCutOff);
	Iir::Butterworth::BandStop<filterorder> outer_filterBS;
	outer_filterBS.setup(fs,powerlineFrequ,bsBandwidth);
	Iir::Butterworth::HighPass<filterorder> inner_filterHP;
	inner_filterHP.setup(fs,innerHighpassCutOff);
	Iir::Butterworth::BandStop<filterorder> inner_filterBS;
	inner_filterBS.setup(fs,powerlineFrequ,bsBandwidth);
	
	Fir1 lms_filter(outerDelayLineLength);
	lms_filter.setLearningRate(LMS_LEARNING_RATE);
	
	fprintf(stderr,"inner_gain = %f, outer_gain = %f, remover_gain = %f\n",inner_gain,outer_gain,remover_gain);

	// main loop processsing sample by sample
	while (!p300_infile.eof()) {
		count++;
		//get the data from .tsv files:

		//SIGNALS
		double inner_raw_data = 0, outer_raw_data = 0, p300trigger = 0;
		if (NULL == filename) {
			p300_infile >> inner_raw_data >> outer_raw_data >> p300trigger;
		} else {
			std::string line;
			std::getline(p300_infile, line);
			vector<string> row_values;
			split(line, '\t', row_values);
			if (row_values.size()>7) {
				inner_raw_data = boost::lexical_cast<double>(row_values[7]);
				outer_raw_data = boost::lexical_cast<double>(row_values[8]);
			}
			p300trigger = 0;
		}
		
		//A) INNER ELECTRODE:
		//1) ADJUST & AMPLIFY
		const double inner_raw = inner_gain * inner_raw_data;
		double inner_filtered = inner_filterHP.filter(inner_raw);
		inner_filtered = inner_filterBS.filter(inner_filtered);

		//3) DELAY
		inner_delayLine.push_back(inner_filtered);
		const double inner = inner_delayLine[0];
		
		innertrigger_delayLine.push_back(p300trigger);
		const double delayedp300trigger = innertrigger_delayLine[0];

		//B) OUTER ELECTRODE:
		//1) ADJUST & AMPLIFY
		const double outer_raw = outer_gain * outer_raw_data;
		const double outerhp = outer_filterHP.filter(outer_raw);
		const double outer = outer_filterBS.filter(outerhp);

		//3) DELAY LINE
		for (int i = outerDelayLineLength-1 ; i > 0; i--) {
			outer_delayLine[i] = outer_delayLine[i-1];
			
		}
		
		outer_delayLine[0] = outer / (double)outerDelayLineLength;
		
		// OUTER INPUT TO NETWORK
		NNO.setInputs(outer_delayLine);
		NNO.propInputs();
		
		// REMOVER OUTPUT FROM NETWORK
		double remover = NNO.getOutput(0) * remover_gain;
		double f_nn = inner - remover;
		
		// FEEDBACK TO THE NETWORK 
		NNO.setError(f_nn);
		NNO.propErrorBackward();
		
		if (count > (samplesNoLearning+outerDelayLineLength)){
			// LEARN
			NNO.setLearningRate(w_eta, 0);
			NNO.updateWeights();
		}
		
#ifdef SAVE_WEIGHTS
		// SAVE WEIGHTS
		NNO.snapWeights(outpPrefix, "p300", subjIndex);
#endif
		wdistance_file << NNO.getWeightDistance();
		for(int i=0; i < NLAYERS; i++ ) {
			wdistance_file << "\t" << NNO.getLayerWeightDistance(i);
		}
		wdistance_file << endl;

		// Do Laplace filter
		double laplace = inner - outer;

		// Do LMS filter
		double corrLMS = lms_filter.filter(outer);
		double lms_output = inner - corrLMS;
		if (count > (samplesNoLearning+outerDelayLineLength)){
			lms_filter.lms_update(lms_output);
		}
		
		// SAVE SIGNALS INTO FILES
		laplace_file << laplace << "\t" << delayedp300trigger << endl;
		// undo the gain so that the signal is again in volt
		inner_file << inner/inner_gain << "\t" << delayedp300trigger << endl;
		outer_file << outer/outer_gain << "\t" << delayedp300trigger << endl;
		remover_file << remover/inner_gain << endl;
		nn_file << f_nn/inner_gain << "\t" << delayedp300trigger << endl;
		lms_file << lms_output/inner_gain << "\t" << delayedp300trigger << endl;
		lms_remover_file << corrLMS/inner_gain << endl;
		
		// PUT VARIABLES IN BUFFERS
		// 1) MAIN SIGNALS
		oo_buf.push_back(outer);
		io_buf.push_back(inner);
		ro_buf.push_back(remover);
		f_nno_buf.push_back(f_nn);
		// 2) LMS outputs
		lms_o_buf.push_back(lms_output);
		
		// PUTTING BUFFERS IN VECTORS FOR PLOTS
		// MAIN SIGNALS
		std::vector<double> oo_plot(oo_buf.begin(), oo_buf.end());
		std::vector<double> io_plot(io_buf.begin(), io_buf.end());
		std::vector<double> ro_plot(ro_buf.begin(), ro_buf.end());
		std::vector<double> f_nno_plot(f_nno_buf.begin(), f_nno_buf.end());
		// LMS outputs
		std::vector<double> lms_o_plot(lms_o_buf.begin(), lms_o_buf.end());
		
#ifdef DoShowPlots
		frame = cv::Scalar(60, 60, 60);
		if (0 == (count % 10)) {
			plots.plotMainSignals(oo_plot,
					      io_plot,
					      ro_plot,
					      f_nno_plot,
					      lms_o_plot, 1);
			plots.plotTitle(sbjct, count, round(count / fs));
			cvui::update();
			cv::imshow(WINDOW, frame);

			if (cv::waitKey(1) == ESC_key) {
				break;
			}
		}
#endif
	}
	NNO.snapWeights(outpPrefix, "p300", subjIndex);
	p300_infile.close();
	remover_file.close();
	nn_file.close();
	inner_file.close();
	outer_file.close();
	lms_file.close();
	laplace_file.close();
	lms_remover_file.close();
	wdistance_file.close();
#ifdef SAVE_WEIGHTS
	weight_file.close();
#endif	
	cout << "The program has reached the end of the input file" << endl;
}



int main(int argc, const char *argv[]) {
	if (argc < 2) {
		fprintf(stderr,"Usage: %s [-a] <subjectNumber>\n",argv[0]);
		fprintf(stderr,"       -a calculates all 20 subjects in a loop.\n");
		fprintf(stderr,"       Press ESC in the plot window to cancel the program.\n");
		return 0;
	}
	const char *filename = NULL;
	if (argc > 2) {
		filename = argv[2];
	}
	if (strcmp(argv[1],"-a") == 0) {
		for(int i = 0; i < nSubj; i++) {
			processOneSubject(i+1,filename);
		}
		return 0;
	}
	const int subj = atoi(argv[1]);
	if ( (subj < 1) || (subj > nSubj) ) {
		fprintf(stderr,"Subj number of out range.\n");
		return -1;
	}
	processOneSubject(subj,filename);
	return 0;
}

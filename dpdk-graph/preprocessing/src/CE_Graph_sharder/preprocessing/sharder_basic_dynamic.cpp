/**
 * @file
 * @author  Xiuneng Wang <xiunengwang@hust.edu.cn>
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2014] [Yongli Cheng , Xiuneng Wang / Huazhong University of Science and Technology]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 
 *
 * @section DESCRIPTION
 *
 * Sharder_basic can convert graphs from the edgelist and adjacency
 * list representations to shards used by the CE_Graph Ce system.
 */

#include <iostream>
#include <stdlib.h>
#include <string>
#include <assert.h>

#define DYNAMICEDATA 1

#include "logger/logger.hpp"
#include "preprocessing/conversions.hpp"
#include "preprocessing/sharder.hpp"
#include "util/cmdopts.hpp"

using namespace CE_Graph;

int opti_times;


int main(int argc, const char ** argv) {
    CE_Graph_init(argc, argv);
    
    global_logger().set_log_level(LOG_DEBUG);
    
	std::string run_mode = get_option_string_interactive("mode", "'main' mode or 'sub' mode");
	
	if(run_mode == "main")
	{
		std::string basefile = get_option_string_interactive("file", "[path to the input graph]");
		std::string edge_data_type = get_option_string_interactive("edgedatatype", "int, uint, short, float, char, double, boolean, long, float-float, int-int, length16 , length24 , length32 , length64 , none");
		std::string nshards_str = get_option_string_interactive("nshards", "Number of shards to create, or 'auto'");
		std::string opti_mode = get_option_string_interactive("opti_mode" , "Average --- 1  , Original --- 2 , Patition --- 3");
		opti_times = atoi(opti_mode.c_str()) - 2;
		if(opti_times > 0)
		{
			std::string opti_times_s = get_option_string_interactive("optimization times" , "times of optimizing the patition of graph , this would delay running time");
			opti_times = atoi(opti_times_s.c_str());
		}
		
		if (edge_data_type == "float") {
			convert<float, float>(basefile, nshards_str);
		} if (edge_data_type == "float-float") {
			convert<PairContainer<float>, PairContainer<float> >(basefile, nshards_str);
		} else if (edge_data_type == "int") {
			convert<int, int>(basefile, nshards_str);
		} else if (edge_data_type == "uint") {
			convert<unsigned int, unsigned int>(basefile, nshards_str);
		} else if (edge_data_type == "int-int") {
			convert<PairContainer<int>, PairContainer<int> >(basefile, nshards_str);
		} else if (edge_data_type == "short") {
			convert<short, short>(basefile, nshards_str);
		} else if (edge_data_type == "double") {
			convert<double, double>(basefile, nshards_str);
		} else if (edge_data_type == "char") {
			convert<char, char>(basefile, nshards_str);
		} else if (edge_data_type == "boolean") {
			convert<bool, bool>(basefile, nshards_str);
		} else if (edge_data_type == "long") {
			convert<long, long>(basefile, nshards_str);
		} else if (edge_data_type == "length16"){
			convert<length16 , length16>(basefile,nshards_str);
		} else if (edge_data_type == "length24"){
			convert<length24 , length24>(basefile,nshards_str);
		} else if (edge_data_type == "length32"){
			convert<length32 , length32>(basefile,nshards_str);
		} else if (edge_data_type == "length64"){
			convert<length64 , length64>(basefile,nshards_str);
		} else if (edge_data_type == "none") {
			convert_none(basefile, nshards_str);
		} else {
			logstream(LOG_ERROR) << "You need to specify edgedatatype. Currently supported: int, short, float, char, double, boolean, long.";
			return -1;    
		}
	}
	else
	{
		std::string edge_data_type = get_option_string_interactive("edgedatatype", "int, uint, short, float, char, double, boolean, long, float-float, int-int, length16 , length24 , length32 , length64 , none");
		shard_sub_mode ssm(edge_data_type);
		ssm.submode();
	}
    
    return 0;
}





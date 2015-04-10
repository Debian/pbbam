# Copyright (c) 2014-2015, Pacific Biosciences of California, Inc.
#
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted (subject to the limitations in the
# disclaimer below) provided that the following conditions are met:
#
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
#  * Redistributions in binary form must reproduce the above
#    copyright notice, this list of conditions and the following
#    disclaimer in the documentation and/or other materials provided
#    with the distribution.
#
#  * Neither the name of Pacific Biosciences nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
# GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY PACIFIC
# BIOSCIENCES AND ITS CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
# WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL PACIFIC BIOSCIENCES OR ITS
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
# USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
# OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# Author: Derek Barnett

test_case("EndToEnd_Placeholder", {
	
	inputFn     <- paste(test_data_path, "ex2.bam", sep="/")
	generatedFn <- paste(test_data_path, "generated.bam", sep="/")

	# loop over original file, store names, write to generated file
	file <- BamFile(inputFn)
	assertTrue(file$IsOpen())

	writer <- BamWriter(generatedFn, file$Header())
	assertTrue(writer$IsOpen())

	entireFile <- EntireFileQuery(file)

	# iterate
	names_in <- list()
	iter <- entireFile$begin()
	end <- entireFile$end()
	while ( iter$'__ne__'(end) ) {
		record <- iter$value()
		names_in <- c(names_in, record$FullName())
		writer$Write(record)
		iter$incr()
	}
	writer$Close() # ensure we flush buffers

	# open generated file, read names back
	file$Open(generatedFn)
	assertTrue(file$IsOpen())

	entireFile <- EntireFileQuery(file)
	
	names_out <- list()
	iter <- entireFile$begin()
	end <- entireFile$end()
	while ( iter$'__ne__'(end) ) {
		names_out <- c(names_out, iter$FullName())
		iter$incr()
	}

	# ensure equal
	assertEqual(names_in, names_out)
})
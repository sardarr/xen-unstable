#!/bin/sh

##
## Test driver script
##

usage() {
    echo "Usage: $0 [opts] <report>"
    echo "  Where report is a name that will be used for report files"
    echo ""
    echo "  Where opts are:"
    echo "  -d          : do not submit a report for this run"
    echo "  -b          : do not ask any questions (batch mode)"
    echo "  -q          : run a quick test set"
    echo "  -e <email>  : set email address for report"
    echo "  -s <report> : just submit report <report>"
}

# Just submit the report
submit_report() {

    reportfile=$1

    ./lib/XmTestReport/Report.py $reportfile
}

# Generate XML result report from output file
make_result_report() {
    output=$1
    reportfile=$2
    if ! ./lib/XmTestReport/ResultReport.py $output > $reportfile; then
	echo "Unable to generate clean ResultReport"
	echo "Take a look at $report"
	exit 1
    fi
}

# Collect environment information for XML report
make_environment_report() {
    os=$1
    prog=$2
    if ! ./lib/XmTestReport/OSReport.py > $os; then
	echo "Unable to generate clean OSReport"
	echo "Take a look at $os"
	exit 1
    fi
    if ! ./lib/XmTestReport/ProgReport.py > $prog; then
	echo "Unable to generate clean ProgReport"
	echo "Take a look at $prog"
	exit 1
    fi
}

# Check conditions needed to actually run the tests
runnable_tests() {	
    # Make sure we're root
    uid=$(id -u)
    if [ $uid != 0 ]; then 
	echo "ERROR: I must be run as root!"
	exit 1
    fi

    # See if the ramdisk has been built
    rdsize=$(stat -Lc %s ramdisk/initrd.img 2>/dev/null)
    if [ -z "$rdsize" ] || [ $rdsize -le 16384 ]; then
	echo "Cannot find a valid ramdisk.  You need to run \"make\" or"
	echo "copy in a previously-built ramdisk to the ramdisk/ directory"
	exit 1
    fi

    # Figure out the version of the ramdisk link and compare it
    # to what it should be as a cheap way of making sure we're
    # using the right version
    realrd=$(readlink ramdisk/initrd.img)
    eval $(./lib/XmTestReport/xmtest.py)
    rrdver="initrd-${XM_TEST_MAJ}.${XM_TEST_MIN}.img"
    if [ "$realrd" != "$rrdver" ]; then
	echo "Error: ramdisk/initrd.img is from an old version"
	echo "You need to build a ramdisk from at least ${XM_TEST_MAJ}.${XM_TEST_MIN}"
	exit 1
    fi

    # See if xend is running
    if ! xm list >/dev/null 2>&1; then
	echo "'xm list' failed: is xend running?"
	exit 1
    fi

}

# Get contact info if needed
get_contact_info() {
    
    if [ ! -f contact_info ]; then
	if [ "$batch" = "yes" ]; then
	    echo "Unable to read contact_info!"
	    echo "Please run me once interactively before using batch mode!"
	    exit 1
	else
	    echo "Please provide your email address so that we can "
	    echo "contact you if we need further information concerning"
	    echo "your results.  Any information provided will be"
	    echo "kept private.  If you wish to remain anonymous, please"
	    echo "hit [ENTER] now."
	    
	    while ! echo "$EMAIL" | grep -q '@'; do
		echo
		echo -n "Your email address: "
		read EMAIL
		if [ -z $EMAIL ]; then
		    EMAIL="anonymous@somewhere.com"
		fi
	    done
	    echo $EMAIL > contact_info
	fi
    fi
}

# Run the tests
run_tests() {
    output=$1
    echo Running tests...
    TEST_VERBOSE=1 make -k check > $output 2>&1
}

run_tests_quick() {

    output=$1

    create_tests="01_create_basic_pos.test 07_create_mem64_pos.test 10_create_fastdestroy.test 14_create_blockroot_pos.test"
    unpause_tests="01_unpause_basic_pos.test"
    memset_tests="01_memset_basic_pos.test 03_memset_random_pos.test"
    help_tests="06_help_allcmds.test"
    testgroups="create unpause memset help"

    echo "*** Quick test" > $output
    for group in $testgroups; do
	eval $(echo list=\$${group}_tests)
	echo "*** Running tests [$list] from $group"
	(cd tests/$group && TEST_VERBOSE=1 make -k check TESTS="$list") >> $output 2>&1
    done

}

# Generate some plain-text reports
make_text_reports() {
    passfail=$1
    failures=$2
    output=$3
    reportfile=$4
    summary=summary.tmp
    echo "Making PASS/FAIL report ($passfail)..."
    cat $OUTPUT | egrep '(REASON|PASS|FAIL|XPASS|XFAIL|SKIP)' | perl -pe 's/^(PASS|FAIL|XPASS|XFAIL)(.+)$/$1$2\n/' > $passfail
    
    echo "Making FAIL report ($failures)..."
    cat $passfail | egrep '(REASON|FAIL)' > $failures
    
    NUMPASS=`grep -c PASS $output`
    NUMFAIL=`grep -c FAIL $output`
    NUMXPASS=`grep -c XPASS $output`
    NUMXFAIL=`grep -c XFAIL $output`
    cat > $summary << EOF
Xm-test execution summary:
  PASS:  $NUMPASS
  FAIL:  $NUMFAIL
  XPASS: $NUMXPASS
  XFAIL: $NUMXFAIL
EOF
    
    cat $summary > $reportfile
    
    echo -e '\n\nDetails:\n' >> $reportfile
    
    ./mkreport $passfail >> $reportfile

    rm $summary
}

############
### Main ###
############

# Defaults
MAXFAIL=10
report=yes
batch=no
run=yes

# Resolve options
while [ $# -gt 0 ]
  do
  case "$1" in
      -d)
	  echo "(Skipping report submission)"
	  report=no
	  ;;
      -b)
	  echo "(Batch mode)"
	  batch=yes
	  ;;
      -e)
	  shift
	  echo $1 > contact_info
	  echo "(Email set to $1)"
	  ;;
      -q)
	  run=quick
	  ;;
      -s)
	  run=no
	  ;;
      *)
	  REPORT=$1
	  break
	  ;;
  esac
  shift
done

# Usage
if [ -z $REPORT ]; then
	usage
	exit 1
fi

# Output files
OSREPORTTEMP=${REPORT}.os.xml
PROGREPORTTEMP=${REPORT}.prog.xml
RESULTREPORTTEMP=${REPORT}.result.xml
XMLREPORT=${REPORT}.xml
OUTPUT=${REPORT}.output
SUMMARY=${REPORT}.summary
PASSFAIL=${REPORT}.passfail
TXTREPORT=${REPORT}.report
FAILURES=${REPORT}.failures
	
#  Make sure permissions are correct
chmod a+x lib/XmTestReport/*
chmod a+x mkreport mergereport

if [ ! -f contact_info ]; then
    if [ "$batch" = "yes" ]; then
	echo "Unable to read contact_info"
	echo "You must run me interactively once!"
	exit 1
    else
	get_contact_info
    fi
fi

if [ "$run" != "no" ]; then
    runnable_tests
    make_environment_report $OSREPORTTEMP $PROGREPORTTEMP
    if [ "$run" = "yes" ]; then
	run_tests $OUTPUT
    else
	run_tests_quick $OUTPUT
    fi
    make_text_reports $PASSFAIL $FAILURES $OUTPUT $TXTREPORT
    make_result_report $OUTPUT $RESULTREPORTTEMP
    cat $OSREPORTTEMP $PROGREPORTTEMP $RESULTREPORTTEMP > $XMLREPORT
    rm $OSREPORTTEMP $PROGREPORTTEMP $RESULTREPORTTEMP
fi

if [ "$report" = "yes" ] && [ "$run" = "yes" ]; then
    if [ ! -f "$XMLREPORT" ]; then
	echo "No such file: $XMLREPORT"
	exit 1
    fi
    submit_report $XMLREPORT
fi

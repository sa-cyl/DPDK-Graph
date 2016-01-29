struct task {
        int interval;
        int iter;
        int machine;
};
struct runcontrol {
	int windowpos;
	int intervals;
	task *T;
};
struct workercontrol {
	int fd_master_server;
	int fd_master_client;
};
struct intervallock {
	int interval;
	char lock[1024];
	//char lock[2P - 1];   	//'1'--locked, '0'--ulock
				//lock[P + 1]--lock[2P-1]: use to the blocks of column
};

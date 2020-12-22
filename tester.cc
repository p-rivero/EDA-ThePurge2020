#include <iostream>
#include <fstream>
#include <cmath>
#include <unistd.h>
#include <string>
#include <sys/wait.h>

using namespace std;

char* my_program;
char* test_against;
bool mode_1v3;

const double qnorm_95 = 1.644854;

void run_game(int i) {
    char command[256];
    
    srand(i+rand());

    char* second_player;
    if (mode_1v3) second_player = test_against;
    else second_player = my_program;

    sprintf(command, "./Game %s %s %s %s < default.cnf > /dev/null 2> /tmp/Auto-tester/out%i.txt -s %i", 
        my_program, second_player, test_against, test_against, i, rand());

    system(command);
}

int main(int argc, char** argv) {
    srand (time(NULL));

    if (argc < 5) {
        cout << "Usage: ./tester num_iterations my_player test_against mode" << endl;
        cout << "Available modes: 1v3 (test against 25%), 2v2 (test against 50%)" << endl;
        cout << "Example: ./tester 2000 Eldar My_Old_AI 1v3" << endl;
        exit(0);
    }

    int num_iterations = atoi(argv[1]);
    my_program = argv[2];
    test_against = argv[3];
    if (string(argv[4]) == "1v3") mode_1v3 = true;
    else if (string(argv[4]) == "2v2") mode_1v3 = false;
    else {
        cerr << "Error: Unsupported mode. Supported modes: 1v3 2v2" << endl;
        exit(1);
    }
    bool silent = (argc > 5 and string(argv[5]) == "-s"); // -s flag used: silence info messages

    system("mkdir /tmp/Auto-tester");

    if (not silent) cout << "running " << num_iterations << " games..." << endl;

    for (int i = 0; i < num_iterations; i++) {
        if (fork() == 0) {
            run_game(i);
            exit(i);
        }
    }

    while (waitpid(-1, NULL, 0) > 0);

    char buff[128];
    cout << "WON GAMES: " << flush;
    sprintf(buff, "grep '%s got top score' /tmp/Auto-tester/out*.txt | wc -l", my_program);
    system(buff);

    system("rm -r /tmp/Auto-tester");
    if (silent) return 0; // -s flag: only show results

    float expected = 0.5;
    if (mode_1v3) expected = 0.25;
    cout << "expected (" << 100*expected << "%): " << num_iterations*expected << endl;
    float standard_error = sqrt(expected * (1-expected) / num_iterations);
    cout << "critical point (better with 95% confidence): " << (qnorm_95*standard_error + expected) * num_iterations << endl;
}
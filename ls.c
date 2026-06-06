
// main function that takes no arguments, returns an int. 0 = success
// #include <stdio.h>
// int main(void){
//     printf("hello from world");
//     return 0;
// }

#include <stdio.h>

// to take parameters we need to change it from void to int argc, char *argv[]
// int argc = means the argument count, how many words when they ran the program
// char *argv[] = a pointer to a pointer of an array of strings, in C strings are an array of chars
int main(int argc, char *argv[]){
    printf("hello from world \n");
    // print: take the first argument, which is always the name of the program
    char *first_arg = argv[1];
    if (first_arg == NULL){
        printf("Warning: no argument passed. \n");
        return 0;
    }
    printf("%s \n", first_arg);
    return 0;
}

// Entry point for run_plain.sh: the tests' app_main without IDF.
void app_main(void);

int main(void)
{
    app_main(); // exits with the Unity result
    return 1;
}

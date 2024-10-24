# This repository is a **fork** of [mu*t*able](https://github.com/mutable-org/mutable) including the Result DB changes.

This page gives a step-by-step instruction on how to carry out the `Result DB` experiments.
Some of the experiments require `mutable`.
You can either build `mutable` manually or use the provided Docker compose file `compose.yaml` to automate the process.

## Set up using `compose.yaml`
* Ensure there are no incomplete builds of mutable before setting up Docker with the provided `compose.yaml` file, i.e., `the mutable_fork_folder` remains unchanged from its state after the download.
* Start your Docker daemon.
* Create a `data` folder that will contain the PostgreSQL files.
    ```console
    $ mkdir data
    ```
* Build the Docker image and start the containers.
    ```console
    $ docker compose up --detach
    ```
    Note that the build process can take a significant amount of time, as building V8 -- required by mutable -- can be
    time-consuming (up to one hour on our machines).

    After the build process is finished, the Docker image `mutable` contains a working installation of mutable (in release mode) as well as a virtual environment containing all required Python packages.
    The Docker image `postgres` contains a running instance of PostgreSQL version 16.2.
* Specify the PostgreSQL parameters.
    On your local machine, the`./data` folder contains the configuration file `postgresql.conf`.
    Set the _shared\_buffers_ value to 16 GiB and _work\_mem_ to 1 GiB inside the configuration file.
    Afterward, restart the postgres server by restarting the `postgres` Docker container.
    ```console
    $ docker restart postgres
    ```
* Start an interactive shell in the `mutable` Docker container.
    ```console
    $ docker exec -it mutable /bin/bash
    ```
* Download the IMDb dataset. The following command must be executed within the Docker container.
    ```console
    $ pipenv run python ./benchmark/get_data.py job
    ```
* Set up the PostgreSQL database. The following command must be executed within the Docker container.
    ```console
    $ ./benchmark/result-db/data/imdb/setup_postgres.sh postgres
    ```
* **Note:** The PostgreSQL username inside the Docker container is `postgres`.

## Set up manually
All the following commands have to be executed from the **mutable_fork_resultdb** project folder!

* Set up a Python virtual environment using [pipenv](https://pipenv.pypa.io/en/latest/).
    ```console
    $ pipenv sync --python 3.10
    ```
    This installs all required Python packages for the utility scripts and visualization.
    In case of problems with `pygraphviz` on macOS, check this [doc](https://anonymous.4open.science/r/mutable-ResultDB-BFE6/mutable_fork_resultdb/doc/preliminaries.md#pipenv).

* Download the IMDb dataset.
    ```console
    $ pipenv run python ./benchmark/get_data.py job
    ```
    This downloads the IMDb dataset to `./benchmark/job/data/`.

* Install and set up PostgreSQL.
    - Install [PostgreSQL](https://www.postgresql.org/) for your operating system. Note, that the experiments in the paper
    were conducted with PostgreSQL version 16.2.

    - Once installed, set the _shared\_buffers_ value to 16 GiB and _work\_mem_ to 1 GiB. There are multiple ways to [set parameters](https://www.postgresql.org/docs/current/config-setting.html).

        For the experiments, we changed the parameters via the configuration file `postgresql.conf`.
        The configuration file is usually contained in the database cluster's data directory, e.g. `/usr/local/var/postgresql/` on macOS Intel or `/var/lib/postgres/data/` on Linux.
        Make sure to restart your server after changing the values.

    - Create the database and import the data. You may need to make the file executable.
        ```console
        $ chmod +x ./benchmark/result-db/data/imdb/setup_postgres.sh
        $ ./benchmark/result-db/data/imdb/setup_postgres.sh <username>
        ```

* Build mutable with the WebAssembly backend
    - Make sure to have all required
      [prerequisites](https://anonymous.4open.science/r/mutable-ResultDB-BFE6/mutable_fork_resultdb/doc/preliminaries.md).
    - Building mutable requires a git repository. Therefore, you have to make the **mutable_fork_resultdb** folder an artificial repository.
      ```console
      $ git init
      $ git add README.md
      $ git commit -m "dummy commit"
      ```
    - Set up mutable by following these
      [instructions](https://anonymous.4open.science/r/mutable-ResultDB-BFE6/mutable_fork_resultdb/doc/setup.md#build-mutable).
      Note, that you have to build mutable with its WebAssembly-based backend.

## Executing the Experiments
After successful setup, the following experiments can be run either within the interactive Docker shell or directly on your local system.
* When using Docker, ensure you are in the interactive Docker shell.
    ```console
    $ docker exec -it mutable /bin/bash
    ```
* When using your local system, ensure that all the following commands are executed from the **mutable_fork_resultdb**
  project folder.

### Result Set Sizes
The corresponding experiments can be found in `./result-set-sizes/`.
* Join Order Benchmark

    Execute the following script.
    ```console
    $ pipenv run python ./benchmark/result-db/result-set-sizes/compute_result_set_size.py -u <username>
    ```
    This script creates `sql` files for each JOB query in `./benchmark/result-db/result-set-sizes/job/<query>`.
    Each file computes the number of result rows and the size of the required attributes for a specific relation.
    The SQL queries are executed using PostgreSQL and the results are written to
    `./benchmark/result-db/result-set-sizes/job/result-set-sizes.csv`.
* Synthetic Star Schema

    Since we exactly know how our results look like for different selectivity values, we do not have to manually
      compute the result sets. We can just calculate the result sets as done in the Visualization notebook.

### Rewrite Methods
* Join Order Benchmark
    - To generate the rewrite methods, execute the following script.
        ```console
        $ pipenv run python ./benchmark/result-db/rewrite-methods/generate_rewrite_methods.py -q imdb -o benchmark/result-db/rewrite-methods/job/ --no-data-transfer
        ```
        This script creates `sql` files for each JOB query in `./benchmark/result-db/rewrite-methods/job/<query>`.
        Each file corresponds to either the default query or one of the rewrite methods.
    - Run each query using the following command. **Make sure to add your PostgreSQL username!**
        ```console
        $ pipenv run python ./benchmark/result-db/rewrite-methods/run.py -u <username> -d imdb -n 5 -q imdb --directory ./benchmark/result-db/rewrite-methods/job/
        ```
    - The results can be found in `./benchmark/result-db/rewrite-methods/job/rewrite-results.csv`.

### Result DB Algorithm
* Join Order Benchmark
    - Generate the real cardinalities for injection into mutable. **Make sure to add your PostgreSQL username!**
        ```console
        $ pipenv run python ./benchmark/result-db/algorithm/create_injected_cardinalities.py -u <username> -d imdb -q imdb -o ./benchmark/result-db/algorithm/job/
        ```
    - Run the benchmark scripts. You may need to make the file executable.
        ```console
        $ chmod +x ./benchmark/result-db/algorithm/run_benchmarks.sh
        $ ./benchmark/result-db/algorithm/run_benchmarks.sh job
        ```
    - The results are individually written to `./benchmark/result-db/algorithm/job/<query>_results.csv`.

### Post-join
* Join Order Benchmark
    - Generate the post-join files. This includes the reduced base tables, real cardinalities, benchmark script for
      execution in mutable, and (setup) files for the execution in PostgreSQL.
        ```console
        $ pipenv run python ./benchmark/result-db/post-join/generate_post-join.py -u <username> -d imdb -q imdb -o ./benchmark/result-db/post-join/job/
        ```
    - Run the PostgreSQL benchmarks. **Make sure to add your PostgreSQL username!**
        ```console
        $ pipenv run python ./benchmark/result-db/post-join/run_postgres.py -u <username> -d imdb -n 5 -q imdb --directory ./benchmark/result-db/post-join/job/
        ```
    - Run the mutable benchmarks.
        ```console
        $ pipenv run python ./benchmark/result-db/post-join/run_mutable.py --workload job
        ```
    - The mutable results are written to `./benchmark/result-db/post-join/job/<query>/<query>_results.csv`. The
      PostgreSQL results are written to `./benchmark/result-db/post-join/job/postjoin-postgres-results.csv`.

## Visualization
All figures and data (i.e., result set sizes, overheads, and end-to-end runtime) presented in the paper can be found in the `Visualization.ipynb` notebook located in `./benchmark/result-db/visualization/`.

* Navigate to the Visualization folder.
    ```console
    $ cd ./benchmark/result-db/visualization/
    ```
* Start the Jupyter server.
    - On your local system.
        ```console
        $ pipenv run jupyter notebook
        ```
        The Jupyter notebook should open in your default browser.
    - In Docker.
        ```console
        $ pipenv run jupyter notebook --ip=0.0.0.0 --no-browser --allow-root
        ```
        Copy the link at the bottom containing the IP `127.0.0.1` (localhost) from the terminal and paste it into the
        address bar of your browser.

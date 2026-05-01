import psycopg
import random
import time

# DB_CONFIG_5432 = "dbname=universitydb user=postgres host=localhost port=5432 password=avyay@26"
DB_CONFIG_5433 = "dbname=universitydb user=postgres host=localhost port=5433 password=avyay@26"

student_ids = ['44986', '97102', '24864', '30773', '1970', '800']
instructor_ids = ['63394', '78698', '4033', '6570', '15350']
course_ids = ['630', '442', '78', '30', '13', '23']
dept_names = ['Pol. Sci.', 'English', 'Accounting']

# student_ids = ['44985', '97101', '24865', '30772', '1968', '794']
# instructor_ids = ['63395', '78699', '4034', '6569', '15347']
# course_ids = ['628', '443', '780', '304', '1', '2']
# dept_names = ['Comp. Sci.', 'Physics', 'Biology']


def generate_queries(n=200):
    queries = []

    for _ in range(n):
        table = random.choice(["student", "instructor", "course", "department"])

        if table == "student":
            id_val = random.choice(student_ids)
            q = ("SELECT * FROM student WHERE id = %s;", (id_val,))

        elif table == "instructor":
            id_val = random.choice(instructor_ids)
            q = ("SELECT * FROM instructor WHERE id = %s;", (id_val,))

        elif table == "course":
            id_val = random.choice(course_ids)
            q = ("SELECT * FROM course WHERE course_id = %s;", (id_val,))

        elif table == "department":
            id_val = random.choice(dept_names)
            q = ("SELECT * FROM department WHERE dept_name = %s;", (id_val,))

        queries.append(q)

    return queries


def run_benchmark(conn, queries, label):
    total_start = time.perf_counter()

    with conn:
        with conn.cursor() as cur:
            for i, (sql, params) in enumerate(queries):

                start = time.perf_counter()

                if params:
                    value = str(params[0]).replace("'", "''")
                    final_sql = sql.replace("%s", f"'{value}'")
                else:
                    final_sql = sql

                cur.execute(final_sql)
                cur.fetchall()

                end = time.perf_counter()

                # print(f"[{label}] Query {i+1}: {(end - start)*1000:.3f} ms")

    total_end = time.perf_counter()

    print("\n==============================")
    print(f"{label} TOTAL TIME: {(total_end - total_start):.3f} seconds")
    print("==============================\n")


if __name__ == "__main__":
    queries = generate_queries(200)

    print("Generated 200 queries...\n")

    # conn1 = psycopg.connect(DB_CONFIG_5432)
    # run_benchmark(conn1, queries, "PORT 5432")

    conn2 = psycopg.connect(DB_CONFIG_5433)
    run_benchmark(conn2, queries, "PORT 5433")

    conn3 = psycopg.connect(DB_CONFIG_5433)
    run_benchmark(conn3, queries, "PORT 5433")
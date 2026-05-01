DROP TABLE IF EXISTS course CASCADE;
DROP TABLE IF EXISTS student CASCADE;
DROP TABLE IF EXISTS instructor CASCADE;
DROP TABLE IF EXISTS department CASCADE;
DROP TABLE IF EXISTS cache_lru_meta CASCADE;


CREATE UNLOGGED TABLE department (
    dept_name VARCHAR(20) PRIMARY KEY,
    building  VARCHAR(15),
    budget    NUMERIC(12,2)
);

CREATE UNLOGGED TABLE course(
    course_id VARCHAR(8) PRIMARY KEY,
    title     VARCHAR(50),
    dept_name VARCHAR(20),
    credits   NUMERIC(2,0)
);

CREATE UNLOGGED TABLE instructor(
    id        VARCHAR(5) PRIMARY KEY,
    name      VARCHAR(20) NOT NULL,
    dept_name VARCHAR(20),
    salary    NUMERIC(8,2)
);

CREATE UNLOGGED TABLE student(
    id        VARCHAR(5) PRIMARY KEY,
    name      VARCHAR(20) NOT NULL,
    dept_name VARCHAR(20),
    tot_cred  NUMERIC(3,0)
);

create UNLOGGED table takes(
    ID varchar(5),
    course_id varchar(8),
    sec_id varchar(8),
    semester varchar(6),
    year numeric(4,0),
    grade varchar(2),
    primary key (ID, course_id, sec_id, semester, year)
);

create UNLOGGED table advisor(
    s_ID varchar(5) PRIMARY KEY,
    i_ID varchar(5)
);

create UNLOGGED table time_slot(
    time_slot_id varchar(4),
    day varchar(1),
    start_hr numeric(2) check (start_hr >= 0 and start_hr < 24),
    start_min numeric(2) check (start_min >= 0 and start_min < 60),
    end_hr numeric(2) check (end_hr >= 0 and end_hr < 24),
    end_min numeric(2) check (end_min >= 0 and end_min < 60),
    primary key (time_slot_id, day, start_hr, start_min)
);

create UNLOGGED table prereq(
    course_id varchar(8),
    prereq_id varchar(8),
    primary key (course_id, prereq_id)
);

create UNLOGGED table teaches(
    ID varchar(5),
    course_id varchar(8),
    sec_id varchar(8),
    semester varchar(6),
    year numeric(4,0),
    primary key (ID, course_id, sec_id, semester, year)
);

create UNLOGGED table section(
    course_id varchar(8),
    sec_id varchar(8),
    semester varchar(6)
    check (semester in ('Fall', 'Winter', 'Spring', 'Summer')),
    year numeric(4,0) check (year > 1701 and year < 2100),
    building varchar(15),
    room_number varchar(7),
    time_slot_id varchar(4),
    primary key (course_id, sec_id, semester, year)
);

create UNLOGGED table classroom(
    building varchar(15),
    room_number varchar(7),
    capacity numeric(4,0),
    primary key (building, room_number)
);

CREATE UNLOGGED TABLE cache_lru_meta (
    table_name   TEXT        NOT NULL,
    pk_value     TEXT        NOT NULL,
    last_access  TIMESTAMPTZ NOT NULL DEFAULT now(),
    inserted_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (table_name, pk_value)
);

CREATE INDEX idx_lru_table_access 
ON cache_lru_meta (table_name, last_access);


CREATE OR REPLACE FUNCTION cache_lru_evict(p_table TEXT, p_pk_col TEXT)
RETURNS VOID LANGUAGE plpgsql AS $$
DECLARE
    v_count  INT;
    v_lru_pk TEXT;
    v_sql    TEXT;
BEGIN
    SELECT COUNT(*) INTO v_count
    FROM cache_lru_meta
    WHERE table_name = p_table;

    WHILE v_count >= 20 LOOP
        SELECT pk_value INTO v_lru_pk
        FROM cache_lru_meta
        WHERE table_name = p_table
        ORDER BY last_access ASC
        LIMIT 1;

        EXIT WHEN v_lru_pk IS NULL;

        v_sql := format('DELETE FROM %I WHERE %I::TEXT = $1', p_table, p_pk_col);
        EXECUTE v_sql USING v_lru_pk;

        DELETE FROM cache_lru_meta
        WHERE table_name = p_table AND pk_value = v_lru_pk;

        v_count := v_count - 1;
    END LOOP;
END;
$$;


CREATE OR REPLACE FUNCTION cache_lru_touch(p_table TEXT, p_pk TEXT)
RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    UPDATE cache_lru_meta
    SET last_access = now()
    WHERE table_name = p_table AND pk_value = p_pk;
END;
$$;


CREATE OR REPLACE FUNCTION cache_insert_student(
    p_id VARCHAR, p_name VARCHAR, p_dept_name VARCHAR, p_tot_cred NUMERIC
) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    PERFORM cache_lru_evict('student', 'id');

    INSERT INTO student VALUES (p_id, p_name, p_dept_name, p_tot_cred)
    ON CONFLICT (id) DO UPDATE
    SET name = EXCLUDED.name,
        dept_name = EXCLUDED.dept_name,
        tot_cred = EXCLUDED.tot_cred;

    INSERT INTO cache_lru_meta (table_name, pk_value)
    VALUES ('student', p_id)
    ON CONFLICT (table_name, pk_value) DO UPDATE
    SET last_access = now();
END;
$$;

CREATE OR REPLACE FUNCTION cache_insert_instructor(
    p_id VARCHAR, p_name VARCHAR, p_dept_name VARCHAR, p_salary NUMERIC
) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    PERFORM cache_lru_evict('instructor', 'id');

    INSERT INTO instructor VALUES (p_id, p_name, p_dept_name, p_salary)
    ON CONFLICT (id) DO UPDATE
    SET name = EXCLUDED.name,
        dept_name = EXCLUDED.dept_name,
        salary = EXCLUDED.salary;

    INSERT INTO cache_lru_meta (table_name, pk_value)
    VALUES ('instructor', p_id)
    ON CONFLICT (table_name, pk_value) DO UPDATE
    SET last_access = now();
END;
$$;

CREATE OR REPLACE FUNCTION cache_insert_course(
    p_id VARCHAR, p_title VARCHAR, p_dept_name VARCHAR, p_credits NUMERIC
) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    PERFORM cache_lru_evict('course', 'course_id');

    INSERT INTO course VALUES (p_id, p_title, p_dept_name, p_credits)
    ON CONFLICT (course_id) DO UPDATE
    SET title = EXCLUDED.title,
        dept_name = EXCLUDED.dept_name,
        credits = EXCLUDED.credits;

    INSERT INTO cache_lru_meta (table_name, pk_value)
    VALUES ('course', p_id)
    ON CONFLICT (table_name, pk_value) DO UPDATE
    SET last_access = now();
END;
$$;

CREATE OR REPLACE FUNCTION cache_insert_department(
    p_dept_name VARCHAR, p_building VARCHAR, p_budget NUMERIC
) RETURNS VOID LANGUAGE plpgsql AS $$
BEGIN
    PERFORM cache_lru_evict('department', 'dept_name');

    INSERT INTO department VALUES (p_dept_name, p_building, p_budget)
    ON CONFLICT (dept_name) DO UPDATE
    SET building = EXCLUDED.building,
        budget = EXCLUDED.budget;

    INSERT INTO cache_lru_meta (table_name, pk_value)
    VALUES ('department', p_dept_name)
    ON CONFLICT (table_name, pk_value) DO UPDATE
    SET last_access = now();
END;
$$;


DROP SUBSCRIPTION IF EXISTS university_cache_sub;

CREATE SUBSCRIPTION university_cache_sub
    CONNECTION 'host=localhost port=5432 dbname=universitydb user=postgres password=password'
    PUBLICATION university_cache_pub
    WITH (
        copy_data = false,
        enabled = true,
        synchronous_commit = off
    );
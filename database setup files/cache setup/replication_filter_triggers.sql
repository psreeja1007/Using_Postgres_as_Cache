CREATE OR REPLACE FUNCTION trg_repl_filter_student()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'student' AND pk_value = OLD.id
        ) THEN
            RETURN NULL;
        END IF;

        PERFORM cache_lru_touch('student', NEW.id);
        RETURN NEW;

    ELSIF TG_OP = 'DELETE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'student' AND pk_value = OLD.id
        ) THEN
            RETURN NULL;
        END IF;

        DELETE FROM cache_lru_meta
        WHERE table_name = 'student' AND pk_value = OLD.id;

        RETURN OLD;
    END IF;

    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS repl_filter_student ON student;
CREATE TRIGGER repl_filter_student
BEFORE UPDATE OR DELETE ON student
FOR EACH ROW EXECUTE FUNCTION trg_repl_filter_student();


CREATE OR REPLACE FUNCTION trg_repl_filter_instructor()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'instructor' AND pk_value = OLD.id
        ) THEN
            RETURN NULL;
        END IF;

        PERFORM cache_lru_touch('instructor', NEW.id);
        RETURN NEW;

    ELSIF TG_OP = 'DELETE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'instructor' AND pk_value = OLD.id
        ) THEN
            RETURN NULL;
        END IF;

        DELETE FROM cache_lru_meta
        WHERE table_name = 'instructor' AND pk_value = OLD.id;

        RETURN OLD;
    END IF;

    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS repl_filter_instructor ON instructor;
CREATE TRIGGER repl_filter_instructor
BEFORE UPDATE OR DELETE ON instructor
FOR EACH ROW EXECUTE FUNCTION trg_repl_filter_instructor();



CREATE OR REPLACE FUNCTION trg_repl_filter_course()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'course' AND pk_value = OLD.course_id
        ) THEN
            RETURN NULL;
        END IF;

        PERFORM cache_lru_touch('course', NEW.course_id);
        RETURN NEW;

    ELSIF TG_OP = 'DELETE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'course' AND pk_value = OLD.course_id
        ) THEN
            RETURN NULL;
        END IF;

        DELETE FROM cache_lru_meta
        WHERE table_name = 'course' AND pk_value = OLD.course_id;

        RETURN OLD;
    END IF;

    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS repl_filter_course ON course;
CREATE TRIGGER repl_filter_course
BEFORE UPDATE OR DELETE ON course
FOR EACH ROW EXECUTE FUNCTION trg_repl_filter_course();



CREATE OR REPLACE FUNCTION trg_repl_filter_department()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'UPDATE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'department' AND pk_value = OLD.dept_name
        ) THEN
            RETURN NULL;
        END IF;

        PERFORM cache_lru_touch('department', NEW.dept_name);
        RETURN NEW;

    ELSIF TG_OP = 'DELETE' THEN
        IF NOT EXISTS (
            SELECT 1 FROM cache_lru_meta
            WHERE table_name = 'department' AND pk_value = OLD.dept_name
        ) THEN
            RETURN NULL;
        END IF;

        DELETE FROM cache_lru_meta
        WHERE table_name = 'department' AND pk_value = OLD.dept_name;

        RETURN OLD;
    END IF;

    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS repl_filter_department ON department;
CREATE TRIGGER repl_filter_department
BEFORE UPDATE OR DELETE ON department
FOR EACH ROW EXECUTE FUNCTION trg_repl_filter_department();
# Hint Placement Fix

## 问题描述

之前的实现将 hint 注释插入到 SQL 语句的开头：
```sql
/*+ HashJoin(t1 t2) Leading((t1 t2)) SeqScan(t1) SeqScan(t2) */
SELECT * FROM t1 WHERE c1 IN (SELECT c2 FROM t2);
```

这是**不正确的**。根据 PostgreSQL hint 语法规范，hint 注释必须紧跟在 `SELECT` 关键字之后。

## 正确的格式

```sql
SELECT /*+ HashJoin(t1 t2) Leading((t1 t2)) SeqScan(t1) SeqScan(t2) */
* FROM t1 WHERE c1 IN (SELECT c2 FROM t2);
```

或者更规范的格式（对于 EXPLAIN）：
```sql
EXPLAIN SELECT /*+ HashJoin(t1 t2) Leading((t1 t2)) SeqScan(t1) SeqScan(t2) */
* FROM t1 WHERE c1 IN (SELECT c2 FROM t2);
```

## 修复内容

修改了 `reconstruct_sql_with_positioned_hints()` 函数（pg_outline.c:1806-1860）：

### 修复前
```c
/* Insert [main] hint at beginning if found */
if (main_hint)
{
    appendStringInfo(&result, "/*+ %s */\n", main_hint);
}
/* Append original SQL */
appendStringInfoString(&result, query_string);
```

### 修复后
```c
/* Insert [main] hint after SELECT keyword if found */
if (main_hint)
{
    const char *select_pos;
    const char *sql_ptr = query_string;
    bool found_select = false;

    /* Find SELECT keyword (case-insensitive) */
    while (*sql_ptr)
    {
        /* Skip whitespace and comments */
        while (*sql_ptr && (*sql_ptr == ' ' || *sql_ptr == '\t' || *sql_ptr == '\n' || *sql_ptr == '\r'))
            sql_ptr++;

        /* Check for SELECT keyword (case-insensitive) */
        if ((*sql_ptr == 'S' || *sql_ptr == 's') &&
            (strncasecmp(sql_ptr, "SELECT", 6) == 0 || strncasecmp(sql_ptr, "select", 6) == 0))
        {
            /* Make sure it's a complete word (not part of another identifier) */
            if (!isalnum((unsigned char)sql_ptr[6]) && sql_ptr[6] != '_')
            {
                select_pos = sql_ptr + 6;  /* Position after "SELECT" */
                found_select = true;
                break;
            }
        }

        /* Skip to next potential position */
        if (*sql_ptr)
            sql_ptr++;
    }

    if (found_select)
    {
        /* Copy everything up to and including SELECT */
        appendBinaryStringInfo(&result, query_string, select_pos - query_string);

        /* Insert hint comment after SELECT */
        appendStringInfo(&result, " /*+ %s */", main_hint);

        /* Copy the rest of the SQL */
        appendStringInfoString(&result, select_pos);
    }
    else
    {
        /* Fallback: if SELECT not found, put hint at beginning */
        appendStringInfo(&result, "/*+ %s */\n", main_hint);
        appendStringInfoString(&result, query_string);
    }
}
```

## 实现特点

1. **大小写不敏感**: 支持 `SELECT`、`select`、`Select` 等各种写法
2. **关键字完整性检查**: 确保匹配的是 `SELECT` 关键字，而不是 `SELECTING` 等包含 SELECT 的标识符
3. **空白字符处理**: 正确处理 SELECT 关键字前后的空格、制表符、换行符
4. **回退机制**: 如果找不到 SELECT 关键字（理论上不应该发生），则回退到原来的行为，将 hint 放在开头

## 测试

使用 `test_hint_placement.sql` 测试文件验证修复：

```bash
cd contrib/pg_outline
make && sudo make install
psql -d testdb -f test_hint_placement.sql
```

## 示例输出

修复后的输出：
```
NOTICE: SQL with outline hints inserted at original positions:
SELECT /*+ HashJoin(t1 t2) Leading((t1 t2)) SeqScan(t1) SeqScan(t2) */ * FROM t1 WHERE c1 IN (SELECT c2 FROM t2);
```

这符合 PostgreSQL/pg_hint_plan 的标准 hint 语法。

## 相关文件

- `contrib/pg_outline/pg_outline.c` - 主实现文件
- `contrib/pg_outline/test_hint_placement.sql` - 测试文件

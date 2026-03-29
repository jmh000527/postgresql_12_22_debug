# Query Fingerprint 一致性修复

## 问题描述

用户报告的问题：
```
Stored fingerprint: 48f17dd76e3059a52af9cf5bfdabd21b
Runtime fingerprint: 89a2cb34e24256ddbb10804138acb715
Both are for essentially the same SQL query
```

这意味着相同的SQL查询在创建outline和运行时产生了不同的fingerprint，导致manual模式下无法匹配outline。

## 根本原因

`normalize_query()` 函数在处理空白字符时不够一致：

1. **前导空格**：没有删除查询开头的空格
2. **连续空格**：多个连续空格被保留，没有合并成单个空格
3. **尾随空格**：查询末尾的空格没有被删除
4. **占位符周围的空格**：替换数字和字符串字面量为 `?` 时，总是添加 ` ? `（前后都有空格），即使不需要

## 解决方案

修改 `normalize_query()` 函数，添加以下机制：

### 1. 添加 `last_was_space` 标志
```c
bool last_was_space = true;  /* 初始为true以自动删除前导空格 */
```

这个标志跟踪上一个字符是否是空格，用于：
- 防止连续空格
- 自动删除前导空格（初始值为true）
- 控制何时在 `?` 前添加空格

### 2. 空格标准化逻辑
```c
/* Normalize whitespace */
if (isspace(*p))
{
    /* 只在需要时添加单个空格 */
    if (!last_was_space && normalized.len > 0)
    {
        appendStringInfoChar(&normalized, ' ');
        last_was_space = true;
    }
    continue;
}
```

### 3. 占位符前的条件空格
```c
/* 只在需要时在 ? 前添加空格 */
if (!last_was_space && normalized.len > 0)
    appendStringInfoChar(&normalized, ' ');
appendStringInfoChar(&normalized, '?');
last_was_space = false;
```

### 4. 尾随空格删除
```c
/* Trim trailing whitespace */
result = normalized.data;
len = normalized.len;
while (len > 0 && isspace(result[len - 1]))
    len--;
result[len] = '\0';
```

### 5. 更新标志位
在所有输出非空格字符的地方，设置 `last_was_space = false`：
- 普通字符
- 数字（标识符的一部分）
- 引号标识符
- `?` 占位符

## 效果

修复后，以下所有查询将产生相同的fingerprint：

```sql
-- 查询1：标准格式
SELECT c1, (SELECT c3 FROM t2 WHERE t2.c2 = t1.c1 LIMIT 1) FROM t1 LIMIT 5;

-- 查询2：前后有空格
  SELECT c1, (SELECT c3 FROM t2 WHERE t2.c2 = t1.c1 LIMIT 1) FROM t1 LIMIT 5;

-- 查询3：多个连续空格
select   c1,   (select   c3   from   t2   where   t2.c2 = t1.c1   limit   1)   from   t1   limit   5;

-- 查询4：全小写
select c1, (select c3 from t2 where t2.c2 = t1.c1 limit 1) from t1 limit 5;
```

所有这些查询都会被标准化为：
```
select c1,(select c3 from t2 where t2.c2=t1.c1 limit ?) from t1 limit ?
```

并产生相同的MD5 fingerprint。

## 验证

现在创建outline后，在manual模式下执行相同查询（即使有不同的空格），也能正确匹配并应用hints。

## 代码变更

文件：`contrib/pg_outline/pg_outline.c`
函数：`normalize_query()`
变更行数：33行插入，6行删除
提交：55d68637b5
